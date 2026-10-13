#include "Pipeline.h"
#include "Analyses.h"
#include "Compat.h"

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

namespace irdiff {

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

static std::unique_ptr<TargetMachine> makeTM(Module &M, const PipelineConfig &C,
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

namespace {
struct Frame {
  std::string Pass, Unit;
  bool ModuleUnit = false;
  std::vector<std::pair<const Function *, uint64_t>> Funcs;
  uint64_t ModHash = 0;
  size_t Counts = 0, GlobalCounts = 0;
  bool Infra = false;
};

struct Runner {
  Module &M;
  const PipelineConfig &C;
  PipelineResult &R;
  std::function<void(const PassRun &, const std::vector<std::string> &, bool,
                     bool)>
      OnChange;
  std::vector<Frame> Stack;
  unsigned Index = 0;
  PassInstrumentationCallbacks *PIC = nullptr;

  static size_t globalCounts(const Module &M) {
    return hash_combine(M.global_size(), M.alias_size(), M.ifunc_size(),
                        M.named_metadata_size());
  }

  static size_t counts(const Module &M) {
    return hash_combine(M.size(), M.global_size(), M.alias_size(),
                        M.ifunc_size());
  }

  void before(StringRef P, const Any &IR) {
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

  void after(StringRef P, const Any *IR, bool Invalidated) {
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

  std::string current() const {
    for (auto It = Stack.rbegin(); It != Stack.rend(); ++It)
      if (!It->Infra)
        return "#" + std::to_string(Index + 1) + " " + It->Pass;
    return "";
  }
};
} // namespace

static bool run(Module &M, const PipelineConfig &C, PipelineResult &R,
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

bool runPipeline(Module &M, const PipelineConfig &C, PipelineResult &R,
                 std::string &Err) {
  Runner Rn{M, C, R, {}, {}, 0};
  installRemarkCollector(M.getContext(), R.Remarks,
                         [&Rn] { return Rn.current(); });
  std::unique_ptr<TargetMachine> TM = makeTM(M, C, R.Notes);
  return run(M, C, R, Err, &Rn, TM.get());
}

static std::string sanitize(StringRef S) {
  std::string R;
  for (char C : S)
    R += (isalnum(static_cast<unsigned char>(C)) || C == '.' || C == '_' ||
          C == '-')
             ? C
             : '_';
  return R.substr(0, 80);
}

static bool diffTexts(const std::string &Before, const std::string &After,
                      const DiffOptions &O, ModuleDiff &D, std::string &Err) {
  LoadResult A = loadModuleFromBuffer(Before, "before", false);
  LoadResult B = loadModuleFromBuffer(After, "after", false);
  if (!A.L || !B.L) {
    Err = !A.L ? A.Error : B.Error;
    return false;
  }
  D = diffModules(*A.L, *B.L, O);
  D.Compat.clear();
  return true;
}

static std::vector<std::string> exact(const std::vector<std::string> &Names,
                                      const DiffOptions &O) {
  std::vector<std::string> R;
  for (auto &N : Names) {
    if (!O.Functions.empty() && !matchesAny(O.Functions, N))
      continue;
    std::string E;
    for (char C : N) {
      if (StringRef("*?[]{}\\").contains(C))
        E += '\\';
      E += C;
    }
    R.push_back(E);
  }
  if (R.empty())
    R.push_back("\x01none");
  return R;
}

namespace {
// lazily loaded bitcode copy of the module at one point in the pipeline
struct Snapshot {
  std::unique_ptr<MemoryBuffer> Buf;
  Loaded L;
};
} // namespace

static std::unique_ptr<Snapshot> loadSnapshot(std::string Data, bool Lazy,
                                              std::string &Err) {
  auto S = std::make_unique<Snapshot>();
  S->Buf = MemoryBuffer::getMemBufferCopy(Data, "snapshot");
  S->L.Ctx = std::make_unique<LLVMContext>();
  Expected<std::unique_ptr<Module>> R =
      Lazy ? getLazyBitcodeModule(S->Buf->getMemBufferRef(), *S->L.Ctx, true)
           : parseBitcodeFile(S->Buf->getMemBufferRef(), *S->L.Ctx);
  if (!R) {
    Err = toString(R.takeError());
    return nullptr;
  }
  S->L.M = std::move(*R);
  if (Error E = S->L.M->materializeMetadata()) {
    Err = toString(std::move(E));
    return nullptr;
  }
  S->L.Info.Format = "bitcode";
  return S;
}

static std::string bitcode(const Module &M) {
  std::string Data;
  raw_string_ostream OS(Data);
  WriteBitcodeToFile(M, OS);
  return Data;
}

// clone F with declarations of what it references into a scratch module
static bool cloneFunction(const Function &F, bool KeepDebug, std::string &Out) {
  const Module &Src = *F.getParent();
  Module Tmp("snapshot", F.getContext());
  Tmp.setTargetTriple(Src.getTargetTriple());
  Tmp.setDataLayout(Src.getDataLayout());
  if (KeepDebug) {
    SmallVector<Module::ModuleFlagEntry, 8> Flags;
    Src.getModuleFlagsMetadata(Flags);
    for (auto &E : Flags)
      Tmp.addModuleFlag(E.Behavior, E.Key->getString(), E.Val);
  }
  ValueToValueMapTy VMap;
  Function *NF = Function::Create(F.getFunctionType(), F.getLinkage(),
                                  F.getAddressSpace(), F.getName(), &Tmp);
  VMap[&F] = NF;
  SmallPtrSet<const Constant *, 32> Seen;
  SmallVector<const Constant *, 32> Work;
  auto Visit = [&](const Value *V) {
    if (auto *C = dyn_cast_or_null<Constant>(V))
      if (Seen.insert(C).second)
        Work.push_back(C);
  };
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      for (const Use &U : I.operands())
        Visit(U.get());
  if (F.hasPersonalityFn())
    Visit(F.getPersonalityFn());
  if (F.hasPrefixData())
    Visit(F.getPrefixData());
  if (F.hasPrologueData())
    Visit(F.getPrologueData());
  unsigned Unnamed = 0;
  while (!Work.empty()) {
    const Constant *C = Work.pop_back_val();
    if (auto *BA = dyn_cast<BlockAddress>(C))
      if (BA->getFunction() != &F)
        return false;
    auto *G = dyn_cast<GlobalValue>(C);
    if (!G) {
      for (const Use &U : C->operands())
        Visit(U.get());
      continue;
    }
    if (G == &F)
      continue;
    std::string Name = G->hasName()
                           ? G->getName().str()
                           : "__irdiff_unnamed" + std::to_string(Unnamed++);
    GlobalValue *D;
    if (auto *FT = dyn_cast<FunctionType>(G->getValueType())) {
      Function *DF = Function::Create(FT, GlobalValue::ExternalLinkage,
                                      G->getAddressSpace(), Name, &Tmp);
      if (auto *GF = dyn_cast<Function>(G)) {
        DF->setAttributes(GF->getAttributes());
        DF->setCallingConv(GF->getCallingConv());
      }
      D = DF;
    } else {
      auto *GV = dyn_cast<GlobalVariable>(G);
      D = new GlobalVariable(Tmp, G->getValueType(), GV && GV->isConstant(),
                             GlobalValue::ExternalLinkage, nullptr, Name,
                             nullptr, G->getThreadLocalMode(),
                             G->getAddressSpace());
    }
    VMap[G] = D;
  }
  for (auto [A, NA] : zip(F.args(), NF->args())) {
    NA.setName(A.getName());
    VMap[&A] = &NA;
  }
  if (KeepDebug) {
    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(NF, &F, VMap, CloneFunctionChangeType::DifferentModule,
                      Returns);
    if (NamedMDNode *CU = Tmp.getNamedMetadata("llvm.dbg.cu"))
      if (!CU->getNumOperands())
        Tmp.eraseNamedMetadata(CU);
  } else {
    // clone() skips debug records; self-mapped metadata avoids graph walks
    NF->copyAttributesFrom(&F);
    if (F.hasPersonalityFn())
      NF->setPersonalityFn(MapValue(F.getPersonalityFn(), VMap));
    if (F.hasPrefixData())
      NF->setPrefixData(MapValue(F.getPrefixData(), VMap));
    if (F.hasPrologueData())
      NF->setPrologueData(MapValue(F.getPrologueData(), VMap));
    SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
    F.getAllMetadata(MDs);
    for (auto &[K, N] : MDs)
      if (K != LLVMContext::MD_dbg)
        NF->addMetadata(K, *N);
    for (const BasicBlock &BB : F)
      VMap[&BB] = BasicBlock::Create(F.getContext(), BB.getName(), NF);
    SmallVector<Instruction *, 64> New;
    for (const BasicBlock &BB : F) {
      auto *NB = cast<BasicBlock>(VMap[&BB]);
      for (const Instruction &I : BB) {
        Instruction *NI = I.clone();
        NI->setName(I.getName());
        NI->insertInto(NB, NB->end());
        VMap[&I] = NI;
        New.push_back(NI);
      }
    }
    for (Instruction *NI : New)
      RemapInstruction(NI, VMap, RF_NoModuleLevelChanges);
  }
  if (const Comdat *C = F.getComdat()) {
    Comdat *NC = Tmp.getOrInsertComdat(C->getName());
    NC->setSelectionKind(C->getSelectionKind());
    NF->setComdat(NC);
  }
  if (!KeepDebug)
    StripDebugInfo(Tmp);
  Out = bitcode(Tmp);
  return true;
}

static bool cloneAll(Module &M, const DiffOptions &O,
                     std::map<std::string, std::string> &Fns) {
  for (Function &F : M) {
    if (F.isDeclaration() ||
        (!O.Functions.empty() && !matchesAny(O.Functions, F.getName().str())))
      continue;
    if (!cloneFunction(F, O.CompareDebug, Fns[F.getName().str()]))
      return false;
  }
  return true;
}

static std::set<std::string> names(Module &M, bool Defined) {
  std::set<std::string> R;
  for (Function &F : M)
    if (F.isDeclaration() != Defined)
      R.insert(F.getName().str());
  return R;
}

static bool isModuleCat(const std::string &C) {
  return C == "module" || C == "type" || C == "global" || C == "config" ||
         C == "metadata" || C == "debug" || C == "match";
}

bool tracePasses(Loaded &L, const PipelineConfig &C, const DiffOptions &O,
                 const std::string &SaveDir, std::vector<PassStep> &Steps,
                 PipelineResult &R, std::string &Err) {
  Module &M = *L.M;
  std::string StepErr;
  std::unique_ptr<TargetMachine> TM = makeTM(M, C, R.Notes);
  // per-function snapshots; whole-module bitcode only for module-level state
  std::map<std::string, std::string> Fns;
  bool PerFunction = cloneAll(M, O, Fns);
  std::set<std::string> Decls = names(M, false);
  std::unique_ptr<Snapshot> Whole = loadSnapshot(bitcode(M), true, StepErr);
  if (!Whole) {
    Err = "internal: cannot snapshot the input: " + StepErr;
    return false;
  }
  auto Save = [&](const std::string &Name) {
    std::error_code EC;
    raw_fd_ostream OS(SaveDir + "/" + Name, EC);
    M.print(OS, nullptr);
  };
  if (!SaveDir.empty()) {
    if (auto EC = sys::fs::create_directories(SaveDir)) {
      Err = SaveDir + ": " + EC.message();
      return false;
    }
    Save("0000-input.ll");
  }
  auto Wanted = [&](const std::string &N) {
    return O.Functions.empty() || matchesAny(O.Functions, N);
  };
  auto Merge = [&](ModuleDiff &Into, ModuleDiff &&From, bool ModuleChanges) {
    if (ModuleChanges)
      for (Change &Ch : From.ModuleChanges)
        if (isModuleCat(Ch.Cat) ||
            ((Ch.Cat == "declaration" || Ch.Cat == "intrinsic") &&
             Ch.Kind == "modified"))
          Into.ModuleChanges.push_back(std::move(Ch));
    for (FuncDiff &F : From.Funcs)
      if (!F.Changes.empty())
        Into.Funcs.push_back(std::move(F));
  };
  auto Step = [&](const PassRun &Run, const std::vector<std::string> &Changed,
                  bool ModChanged, bool ModuleLevel) {
    PassStep S;
    S.Run = Run;
    std::vector<std::string> List;
    for (auto &N : Changed)
      if (Wanted(N))
        List.push_back(N);
    if (ModChanged) {
      std::set<std::string> Now = names(M, true);
      for (auto &[N, B] : Fns)
        if (!Now.count(N))
          List.push_back(N);
      for (auto &N : Now)
        if (!Fns.count(N) && Wanted(N))
          List.push_back(N);
      std::set<std::string> NowDecls = names(M, false);
      auto Decl = [&](const std::string &N, bool Added) {
        Change Ch;
        Ch.Cat =
            StringRef(N).starts_with("llvm.") ? "intrinsic" : "declaration";
        Ch.Kind = Added ? "added" : "removed";
        Ch.Significance = Ch.Cat == "intrinsic" ? Sig::Minor : Sig::Major;
        Ch.Subject = "@" + N;
        S.Diff.ModuleChanges.push_back(Ch);
      };
      for (auto &N : Decls)
        if (!NowDecls.count(N) && !Now.count(N))
          Decl(N, false);
      for (auto &N : NowDecls)
        if (!Decls.count(N) && !Fns.count(N))
          Decl(N, true);
      Decls = std::move(NowDecls);
    }
    std::sort(List.begin(), List.end());
    List.erase(std::unique(List.begin(), List.end()), List.end());
    if (List.empty() && !ModuleLevel && S.Diff.ModuleChanges.empty())
      return;
    S.Functions = List;
    for (auto &N : List) {
      Function *F = M.getFunction(N);
      bool Def = F && !F->isDeclaration();
      std::string After;
      if (Def && !cloneFunction(*F, O.CompareDebug, After)) {
        StepErr = "cannot snapshot @" + N;
        return;
      }
      auto It = Fns.find(N);
      std::unique_ptr<Snapshot> A, B;
      if (It != Fns.end())
        A = loadSnapshot(It->second, false, StepErr);
      if (Def)
        B = loadSnapshot(After, false, StepErr);
      if ((It != Fns.end() && !A) || (Def && !B))
        return;
      Loaded None;
      None.Ctx = std::make_unique<LLVMContext>();
      None.M = std::make_unique<Module>("none", *None.Ctx);
      DiffOptions Sub = O;
      Sub.Functions = exact({N}, O);
      Merge(S.Diff, diffModules(A ? A->L : None, B ? B->L : None, Sub), false);
      if (Def)
        Fns[N] = std::move(After);
      else
        Fns.erase(N);
    }
    if (ModuleLevel) {
      std::unique_ptr<Snapshot> Cur = loadSnapshot(bitcode(M), true, StepErr);
      if (!Cur)
        return;
      DiffOptions Sub = O;
      Sub.Functions.clear();
      Sub.ModuleOnly = true;
      Merge(S.Diff, diffModules(Whole->L, Cur->L, Sub), true);
      Whole = std::move(Cur);
    }
    if (!SaveDir.empty()) {
      char Buf[16];
      snprintf(Buf, sizeof(Buf), "%04u", Run.Index);
      Save(std::string(Buf) + "-" + sanitize(Run.Pass) + "-" +
           sanitize(Run.Unit) + ".ll");
    }
    Steps.push_back(std::move(S));
  };
  // whole-module fallback when a function cannot be cloned on its own
  auto WholeStep = [&](const PassRun &Run,
                       const std::vector<std::string> &Funcs, bool ModChanged,
                       bool) {
    if (!O.Functions.empty() && !ModChanged) {
      bool Any = false;
      for (auto &F : Funcs)
        Any |= Wanted(F);
      if (!Any)
        return;
    }
    std::unique_ptr<Snapshot> Cur = loadSnapshot(bitcode(M), true, StepErr);
    if (!Cur)
      return;
    std::vector<std::string> List = Funcs;
    if (ModChanged) {
      auto Old = names(*Whole->L.M, true), New = names(*Cur->L.M, true);
      for (auto &N : Old)
        if (!New.count(N))
          List.push_back(N);
      for (auto &N : New)
        if (!Old.count(N))
          List.push_back(N);
    }
    PassStep S;
    S.Run = Run;
    for (auto &N : List)
      if (Wanted(N))
        S.Functions.push_back(N);
    DiffOptions Sub = O;
    Sub.Functions = exact(List, O);
    S.Diff = diffModules(Whole->L, Cur->L, Sub);
    S.Diff.Compat.clear();
    if (!SaveDir.empty()) {
      char Buf[16];
      snprintf(Buf, sizeof(Buf), "%04u", Run.Index);
      Save(std::string(Buf) + "-" + sanitize(Run.Pass) + "-" +
           sanitize(Run.Unit) + ".ll");
    }
    Whole = std::move(Cur);
    Steps.push_back(std::move(S));
  };
  Runner Rn{M, C, R, {}, {}, 0};
  Rn.OnChange = [&](const PassRun &Run, const std::vector<std::string> &Funcs,
                    bool ModChanged, bool ModuleLevel) {
    if (!StepErr.empty())
      return;
    if (PerFunction)
      Step(Run, Funcs, ModChanged, ModuleLevel);
    else
      WholeStep(Run, Funcs, ModChanged, ModuleLevel);
  };
  if (!PerFunction)
    R.Notes.push_back("a function references another function's block "
                      "address; using whole-module snapshots (slower)");
  installRemarkCollector(M.getContext(), R.Remarks,
                         [&Rn] { return Rn.current(); });
  if (!run(M, C, R, Err, &Rn, TM.get()))
    return false;
  if (!StepErr.empty()) {
    Err = "internal: could not read back a pass snapshot: " + StepErr;
    return false;
  }
  for (PassStep &S : Steps) {
    std::string Tag = "#" + std::to_string(S.Run.Index) + " ";
    for (const Remark &Rm : R.Remarks)
      if (StringRef(Rm.Origin).starts_with(Tag))
        S.Remarks.push_back(Rm);
  }
  return true;
}

bool traceLog(const std::string &Path, const DiffOptions &O,
              std::vector<PassStep> &Steps, std::vector<std::string> &Notes,
              std::string &Err) {
  auto Buf = MemoryBuffer::getFileOrSTDIN(Path);
  if (!Buf) {
    Err = Path + ": cannot read: " + Buf.getError().message();
    return false;
  }
  struct Seg {
    std::string Kind, Pass, Unit;
    std::string Text;
    bool Omitted = false;
  };
  std::vector<Seg> Segs;
  StringRef Rest = (*Buf)->getBuffer();
  while (!Rest.empty()) {
    auto [Line, Next] = Rest.split('\n');
    Rest = Next;
    StringRef T = Line.trim();
    StringRef H = T;
    H.consume_front("; ");
    if (H.starts_with("*** IR Dump") || H.starts_with("*** IR Pass")) {
      Seg S;
      H = H.drop_front(4);
      H.consume_back(" ***");
      if (H.consume_front("IR Dump At Start")) {
        S.Kind = "Start";
      } else if (H.consume_front("IR Dump After ")) {
        S.Kind = "After";
      } else if (H.consume_front("IR Dump Before ")) {
        S.Kind = "Before";
      } else {
        S.Kind = "Other";
        S.Omitted = true;
      }
      if (H.consume_back(" omitted because no change") ||
          H.consume_back(" filtered out") || H.consume_back(" ignored"))
        S.Omitted = true;
      size_t On = H.find(" on ");
      S.Pass = H.substr(0, On).str();
      if (On != StringRef::npos)
        S.Unit = H.substr(On + 4).str();
      Segs.push_back(std::move(S));
      continue;
    }
    if (!Segs.empty())
      Segs.back().Text += Line.str() + "\n";
  }
  if (Segs.empty()) {
    Err = Path + ": no '*** IR Dump' sections found; produce the log with "
                 "-print-after-all (or -print-changed) and -print-module-scope";
    return false;
  }
  std::string Prev;
  bool HavePrev = false;
  unsigned Index = 0;
  for (Seg &S : Segs) {
    ++Index;
    if (S.Omitted || StringRef(S.Text).trim().empty())
      continue;
    StringRef Tx(S.Text);
    if (!Tx.contains("target triple") && !Tx.contains("source_filename") &&
        !Tx.contains("; ModuleID")) {
      Err = Path + ": dump for '" + S.Pass +
            "' is not module-scoped; rerun "
            "with -print-module-scope";
      return false;
    }
    if (S.Kind == "Start" || S.Kind == "Before") {
      Prev = S.Text;
      HavePrev = true;
      continue;
    }
    if (!HavePrev) {
      Notes.push_back("no initial snapshot before '" + S.Pass +
                      "'; its changes cannot be attributed");
      Prev = S.Text;
      HavePrev = true;
      continue;
    }
    PassStep St;
    St.Run.Index = Index;
    St.Run.Pass = S.Pass;
    St.Run.Unit = S.Unit;
    std::string E;
    if (!diffTexts(Prev, S.Text, O, St.Diff, E)) {
      Err = Path + ": cannot parse dump after '" + S.Pass + "': " + E;
      return false;
    }
    bool Any = !St.Diff.ModuleChanges.empty();
    for (auto &F : St.Diff.Funcs)
      Any |= !F.Changes.empty();
    St.Run.Changed = Any;
    Prev = std::move(S.Text);
    if (Any)
      Steps.push_back(std::move(St));
  }
  return true;
}

std::vector<std::string> flattenPipeline(StringRef T) {
  std::vector<std::string> Out, Scope;
  std::string Cur;
  int Angle = 0;
  for (char C : T) {
    if (C == '<')
      ++Angle;
    if (C == '>')
      --Angle;
    if (Angle > 0 || (C != '(' && C != ')' && C != ',')) {
      Cur += C;
      continue;
    }
    std::string Prefix;
    for (auto &S : Scope)
      Prefix += S + "/";
    if (C == '(') {
      Scope.push_back(Cur);
    } else if (!Cur.empty()) {
      Out.push_back(Prefix + Cur);
    }
    if (C == ')' && !Scope.empty())
      Scope.pop_back();
    Cur.clear();
  }
  if (!Cur.empty())
    Out.push_back(Cur);
  return Out;
}

void diffPipelines(const PipelineResult &A, const PipelineResult &B,
                   ModuleDiff &D) {
  auto FA = flattenPipeline(A.PipelineText),
       FB = flattenPipeline(B.PipelineText);
  std::vector<uint64_t> HA, HB;
  for (auto &S : FA)
    HA.push_back(hashStr(S));
  for (auto &S : FB)
    HB.push_back(hashStr(S));
  std::vector<bool> KA(FA.size()), KB(FB.size());
  for (auto [I, J] : lcs(HA, HB)) {
    KA[I] = true;
    KB[J] = true;
  }
  auto Mk = [](StringRef Kind, std::string Subject, Sig S) {
    Change C;
    C.Cat = "pass";
    C.Kind = Kind.str();
    C.Subject = std::move(Subject);
    C.Significance = S;
    return C;
  };
  for (unsigned I = 0; I < FA.size(); ++I)
    if (!KA[I]) {
      Change C = Mk("removed", FA[I], Sig::Major);
      C.Details.push_back("pipeline position " + std::to_string(I + 1) +
                          " in A; absent at this position in B");
      D.PassChanges.push_back(C);
    }
  for (unsigned I = 0; I < FB.size(); ++I)
    if (!KB[I]) {
      Change C = Mk("added", FB[I], Sig::Major);
      C.Details.push_back("pipeline position " + std::to_string(I + 1) +
                          " in B; absent at this position in A");
      D.PassChanges.push_back(C);
    }
  std::map<std::string, std::pair<unsigned, unsigned>> Exec;
  for (auto &R : A.Runs)
    ++Exec[R.Pass].first;
  for (auto &R : B.Runs)
    ++Exec[R.Pass].second;
  std::map<std::string, std::pair<unsigned, unsigned>> Chg;
  for (auto &R : A.Runs)
    Chg[R.Pass].first += R.Changed;
  for (auto &R : B.Runs)
    Chg[R.Pass].second += R.Changed;
  for (auto &[P, N] : Exec)
    if (N.first != N.second || Chg[P].first != Chg[P].second) {
      Change C = Mk("modified", P, Sig::Minor);
      C.Before = std::to_string(N.first) + " runs, " +
                 std::to_string(Chg[P].first) + " changed IR";
      C.After = std::to_string(N.second) + " runs, " +
                std::to_string(Chg[P].second) + " changed IR";
      C.Details.push_back("executions: " + C.Before + " -> " + C.After);
      D.PassChanges.push_back(C);
    }
}

} // namespace irdiff
