#include "Analyses.h"
#include "Compat.h"
#include "PipelineDetail.h"

#include "llvm/ADT/Any.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <map>
#include <mutex>
#include <set>

#define X(T)                                                                   \
  extern "C" void LLVMInitialize##T##TargetInfo();                             \
  extern "C" void LLVMInitialize##T##Target();                                 \
  extern "C" void LLVMInitialize##T##TargetMC();
#include "Targets.def"
#undef X

using namespace llvm;

using namespace llvm;

namespace irdiff {
namespace detail {

static void initTargets() {
  static std::once_flag Once;
  std::call_once(Once, [] {
#define X(T)                                                                   \
  LLVMInitialize##T##TargetInfo();                                             \
  LLVMInitialize##T##Target();                                                 \
  LLVMInitialize##T##TargetMC();
#include "Targets.def"
#undef X
  });
}

std::unique_ptr<TargetMachine> makeTM(Module &M, const PipelineConfig &C,
                                      std::vector<std::string> &Notes) {
  if (!C.UseTarget) {
    Notes.push_back("target machine disabled (--no-target); target-independent "
                    "cost models are used");
    return nullptr;
  }
  std::string TT = tripleOf(M);
  if (TT.empty()) {
    Notes.push_back("module has no target triple; target-independent cost "
                    "models are used");
    return nullptr;
  }
  initTargets();
  std::string Err;
  Triple T(TT);
  const Target *Tgt = lookupTarget(T, Err);
  if (!Tgt) {
    Notes.push_back("target '" + TT + "' is not available in this build (" +
                    Err + "); target-independent cost models are used");
    return nullptr;
  }
  std::unique_ptr<TargetMachine> TM(
      createTM(Tgt, T, C.CPU, C.Features, CodeGenOptLevel::Default));
  if (TM && M.getDataLayoutStr().empty()) {
    M.setDataLayout(TM->createDataLayout());
    Notes.push_back("module had no data layout; using the target default");
  }
  return TM;
}

static bool ignoredPass(StringRef P) {
  static const std::vector<StringRef> S = {"PassManager",
                                           "PassAdaptor",
                                           "AnalysisManagerProxy",
                                           "DevirtSCCRepeatedPass",
                                           "ModuleInlinerWrapperPass",
                                           "VerifierPass",
                                           "PrintModulePass",
                                           "PrintFunctionPass",
                                           "RequireAnalysisPass",
                                           "InvalidateAnalysisPass"};
  return isSpecialPass(P, S);
}

static uint64_t hashFunction(const Function &F) {
  hash_code H = hash_combine(F.getLinkage(), F.getAttributes().getRawPointer(),
                             F.getCallingConv(), F.getFunctionType(),
                             F.isDeclaration(), F.size());
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  for (const BasicBlock &BB : F) {
    H = hash_combine(H, &BB);
    for (const Instruction &I : BB) {
      H = hash_combine(H, &I, I.getOpcode(), I.getType(),
                       I.getRawSubclassOptionalData(), I.getNumOperands());
      for (const Use &U : I.operands())
        H = hash_combine(H, U.get());
      if (auto *C = dyn_cast<CmpInst>(&I))
        H = hash_combine(H, C->getPredicate());
      if (auto *L = dyn_cast<LoadInst>(&I))
        H = hash_combine(H, L->getAlign().value(), L->isVolatile(),
                         L->getOrdering());
      if (auto *S = dyn_cast<StoreInst>(&I))
        H = hash_combine(H, S->getAlign().value(), S->isVolatile(),
                         S->getOrdering());
      if (auto *A = dyn_cast<AllocaInst>(&I))
        H = hash_combine(H, A->getAllocatedType(), A->getAlign().value());
      if (auto *G = dyn_cast<GetElementPtrInst>(&I))
        H = hash_combine(H, G->getSourceElementType());
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        H = hash_combine(H, CB->getCallingConv(),
                         CB->getAttributes().getRawPointer(),
                         CB->getFunctionType());
        if (auto *CI = dyn_cast<CallInst>(&I))
          H = hash_combine(H, CI->getTailCallKind());
      }
      if (auto *SV = dyn_cast<ShuffleVectorInst>(&I))
        H = hash_combine(H, hash_combine_range(SV->getShuffleMask().begin(),
                                               SV->getShuffleMask().end()));
      if (auto *EV = dyn_cast<ExtractValueInst>(&I))
        H = hash_combine(H, hash_combine_range(EV->idx_begin(), EV->idx_end()));
      if (auto *IV = dyn_cast<InsertValueInst>(&I))
        H = hash_combine(H, hash_combine_range(IV->idx_begin(), IV->idx_end()));
      MDs.clear();
      I.getAllMetadata(MDs);
      for (auto &[K, N] : MDs)
        H = hash_combine(H, K, N);
      for (const DbgRecord &R : I.getDbgRecordRange())
        H = hash_combine(H, &R);
    }
  }
  return H;
}

static uint64_t hashModuleLevel(const Module &M) {
  hash_code H = hash_combine(M.size(), M.global_size(), M.alias_size(),
                             M.ifunc_size(), M.named_metadata_size());
  for (const GlobalVariable &G : M.globals())
    H = hash_combine(H, &G, G.getLinkage(),
                     G.hasInitializer() ? G.getInitializer() : nullptr,
                     G.isConstant(), G.getAlign() ? G.getAlign()->value() : 0,
                     G.getUnnamedAddr());
  for (const Function &F : M)
    H = hash_combine(H, &F, F.getLinkage(), F.getAttributes().getRawPointer(),
                     F.isDeclaration());
  return H;
}

static std::vector<const Function *> unitFunctions(const Any &IR,
                                                   const Module &M) {
  std::vector<const Function *> R;
  if (const auto *F = any_cast<const Function *>(&IR))
    R.push_back(*F);
  else if (const auto *L = any_cast<const Loop *>(&IR))
    R.push_back((*L)->getHeader()->getParent());
  else if (const auto *C = any_cast<const LazyCallGraph::SCC *>(&IR))
    for (const LazyCallGraph::Node &N : **C)
      R.push_back(&N.getFunction());
  else
    for (const Function &F : M)
      R.push_back(&F);
  return R;
}

static std::string unitName(const Any &IR) {
  if (const auto *F = any_cast<const Function *>(&IR))
    return "@" + (*F)->getName().str();
  if (const auto *L = any_cast<const Loop *>(&IR)) {
    const BasicBlock *H = (*L)->getHeader();
    return "loop %" + H->getName().str() + " in @" +
           H->getParent()->getName().str();
  }
  if (const auto *C = any_cast<const LazyCallGraph::SCC *>(&IR)) {
    std::string S = "scc(";
    bool First = true;
    for (const LazyCallGraph::Node &N : **C) {
      S += (First ? "@" : ", @") + N.getFunction().getName().str();
      First = false;
    }
    return S + ")";
  }
  return "[module]";
}

static size_t globalCounts(const Module &M) {
  return hash_combine(M.global_size(), M.alias_size(), M.ifunc_size(),
                      M.named_metadata_size());
}

static size_t counts(const Module &M) {
  return hash_combine(M.size(), M.global_size(), M.alias_size(),
                      M.ifunc_size());
}

void Runner::before(StringRef P, const Any &IR) {
  Frame F;
  F.Pass = P.str();
  F.Unit = unitName(IR);
  F.Counts = counts(M);
  F.GlobalCounts = globalCounts(M);
  // managers/adaptors: only catch work done between passes (dead fn cleanup)
  if (ignoredPass(P)) {
    F.Infra = true;
    Stack.push_back(std::move(F));
    return;
  }
  F.ModuleUnit = any_cast<const Module *>(&IR) != nullptr;
  for (const Function *Fn : unitFunctions(IR, M))
    F.Funcs.push_back({Fn, hashFunction(*Fn)});
  F.ModHash = F.ModuleUnit ? hashModuleLevel(M) : 0;
  Stack.push_back(std::move(F));
}

void Runner::after(StringRef P, const Any *IR, bool Invalidated) {
  if (Stack.empty())
    return;
  Frame F = std::move(Stack.back());
  Stack.pop_back();
  if (F.Infra) {
    bool Global = globalCounts(M) != F.GlobalCounts;
    if (counts(M) == F.Counts && !Global)
      return;
    PassRun Run;
    Run.Index = ++Index;
    Run.Pass = F.Pass;
    Run.Unit = F.Unit;
    Run.Changed = true;
    R.Runs.push_back(Run);
    if (OnChange)
      OnChange(Run, {}, true, Global);
    return;
  }
  PassRun Run;
  Run.Index = ++Index;
  Run.Pass = F.Pass;
  if (PIC)
    Run.Short = PIC->getPassNameForClassName(F.Pass).str();
  Run.Unit = F.Unit;
  bool CountsChanged = counts(M) != F.Counts;
  bool ModuleLevel = globalCounts(M) != F.GlobalCounts ||
                     (F.ModuleUnit && hashModuleLevel(M) != F.ModHash);
  bool ModChanged = CountsChanged || Invalidated || ModuleLevel;
  std::vector<const Function *> Fns;
  if (!Invalidated) {
    bool Wide = F.ModuleUnit || any_cast<const LazyCallGraph::SCC *>(IR);
    std::set<const Function *> Live, Before;
    if (Wide || CountsChanged)
      for (const Function &Fn : M)
        Live.insert(&Fn);
    for (auto &[Fn, H] : F.Funcs) {
      Before.insert(Fn);
      if ((Live.empty() || Live.count(Fn)) && hashFunction(*Fn) != H)
        Fns.push_back(Fn);
    }
    if (Wide)
      for (const Function *Fn : unitFunctions(*IR, M))
        if (!Before.count(Fn))
          Fns.push_back(Fn);
    // callers' call sites may change with the callee (e.g. arg promotion)
    if (Wide)
      for (size_t I = 0, E = Fns.size(); I < E; ++I)
        for (const User *U : Fns[I]->users())
          if (auto *CI = dyn_cast<Instruction>(U))
            Fns.push_back(CI->getFunction());
  }
  std::vector<std::string> Changed;
  for (const Function *Fn : Fns)
    if (!is_contained(Changed, Fn->getName().str()))
      Changed.push_back(Fn->getName().str());
  Run.Changed = ModChanged || !Changed.empty();
  R.Runs.push_back(Run);
  if (Run.Changed && OnChange)
    OnChange(Run, Changed, ModChanged, ModuleLevel || Invalidated);
}

std::string Runner::current() const {
  for (auto It = Stack.rbegin(); It != Stack.rend(); ++It)
    if (!It->Infra)
      return "#" + std::to_string(Index + 1) + " " + It->Pass;
  return "";
}

bool run(Module &M, const PipelineConfig &C, PipelineResult &R,
         std::string &Err, Runner *Rn, TargetMachine *TM) {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassInstrumentationCallbacks PIC;
  StandardInstrumentations SI(M.getContext(), false, C.VerifyEach);
  SI.registerCallbacks(PIC, &MAM);
  if (Rn) {
    Rn->PIC = &PIC;
    PIC.registerBeforeNonSkippedPassCallback(
        [Rn](StringRef P, Any IR) { Rn->before(P, IR); });
    PIC.registerAfterPassCallback(
        [Rn](StringRef P, Any IR, const PreservedAnalyses &) {
          Rn->after(P, &IR, false);
        });
    PIC.registerAfterPassInvalidatedCallback(
        [Rn](StringRef P, const PreservedAnalyses &) {
          Rn->after(P, nullptr, true);
        });
  }
  PassBuilder PB(TM, PipelineTuningOptions(), std::nullopt, &PIC);
  FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM;
  if (auto E = PB.parsePassPipeline(MPM, C.Passes)) {
    Err = "invalid pass pipeline '" + C.Passes + "': " + toString(std::move(E));
    return false;
  }
  raw_string_ostream OS(R.PipelineText);
  MPM.printPipeline(OS, [&PIC](StringRef ClassName) {
    StringRef N = PIC.getPassNameForClassName(ClassName);
    return N.empty() ? ClassName : N;
  });
  MPM.run(M, MAM);
  std::string VErr;
  raw_string_ostream VOS(VErr);
  if (verifyModule(M, &VOS)) {
    Err = "the pipeline produced invalid IR:\n" + VErr;
    return false;
  }
  return true;
}

} // namespace detail

using namespace detail;

bool runPipeline(Module &M, const PipelineConfig &C, PipelineResult &R,
                 std::string &Err) {
  Runner Rn{M, C, R, {}, {}, 0};
  installRemarkCollector(M.getContext(), R.Remarks,
                         [&Rn] { return Rn.current(); });
  std::unique_ptr<TargetMachine> TM = makeTM(M, C, R.Notes);
  return run(M, C, R, Err, &Rn, TM.get());
}

} // namespace irdiff
