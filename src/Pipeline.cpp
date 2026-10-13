#include "Pipeline.h"
#include "Analyses.h"
#include "Compat.h"

#include "llvm/ADT/Any.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
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
  size_t Counts = 0;
};

struct Runner {
  Module &M;
  const PipelineConfig &C;
  PipelineResult &R;
  std::function<void(const PassRun &, const std::vector<std::string> &)>
      OnChange;
  std::vector<Frame> Stack;
  unsigned Index = 0;
  PassInstrumentationCallbacks *PIC = nullptr;

  static size_t counts(const Module &M) {
    return hash_combine(M.size(), M.global_size(), M.alias_size(),
                        M.ifunc_size());
  }

  void before(StringRef P, const Any &IR) {
    if (ignoredPass(P))
      return;
    Frame F;
    F.Pass = P.str();
    F.Unit = unitName(IR);
    F.ModuleUnit = any_cast<const Module *>(&IR) != nullptr;
    for (const Function *Fn : unitFunctions(IR, M))
      F.Funcs.push_back({Fn, hashFunction(*Fn)});
    F.ModHash = F.ModuleUnit ? hashModuleLevel(M) : 0;
    F.Counts = counts(M);
    Stack.push_back(std::move(F));
  }

  void after(StringRef P, const Any *IR, bool Invalidated) {
    if (ignoredPass(P) || Stack.empty())
      return;
    Frame F = std::move(Stack.back());
    Stack.pop_back();
    PassRun Run;
    Run.Index = ++Index;
    Run.Pass = F.Pass;
    if (PIC)
      Run.Short = PIC->getPassNameForClassName(F.Pass).str();
    Run.Unit = F.Unit;
    std::vector<std::string> Changed;
    bool ModChanged = counts(M) != F.Counts ||
                      (F.ModuleUnit && hashModuleLevel(M) != F.ModHash);
    if (Invalidated) {
      ModChanged = true;
    } else {
      std::set<const Function *> Live;
      for (const Function &Fn : M)
        Live.insert(&Fn);
      for (auto &[Fn, H] : F.Funcs)
        if (Live.count(Fn) && hashFunction(*Fn) != H)
          Changed.push_back(Fn->getName().str());
    }
    Run.Changed = ModChanged || !Changed.empty();
    R.Runs.push_back(Run);
    if (Run.Changed && OnChange) {
      if (ModChanged)
        Changed.clear();
      OnChange(Run, Changed);
    }
  }

  std::string current() const {
    if (Stack.empty())
      return "";
    return "#" + std::to_string(Index + 1) + " " + Stack.back().Pass;
  }
};
} // namespace

static bool run(Module &M, const PipelineConfig &C, PipelineResult &R,
                std::string &Err, Runner *Rn) {
  std::unique_ptr<TargetMachine> TM = makeTM(M, C, R.Notes);
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
  PassBuilder PB(TM.get(), PipelineTuningOptions(), std::nullopt, &PIC);
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
  return run(M, C, R, Err, &Rn);
}

static std::string printModule(const Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
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
                      const DiffOptions &O,
                      const std::vector<std::string> &Funcs, ModuleDiff &D,
                      std::string &Err) {
  LoadResult A = loadModuleFromBuffer(Before, "before", false);
  LoadResult B = loadModuleFromBuffer(After, "after", false);
  if (!A.L || !B.L) {
    Err = !A.L ? A.Error : B.Error;
    return false;
  }
  DiffOptions Sub = O;
  if (!Funcs.empty()) {
    std::vector<std::string> Exact;
    for (auto &F : Funcs)
      if (O.Functions.empty() || matchesAny(O.Functions, F))
        Exact.push_back(F);
    if (Exact.empty())
      Exact.push_back("\x01none");
    Sub.Functions = Exact;
  }
  D = diffModules(*A.L, *B.L, Sub);
  D.Compat.clear();
  return true;
}

bool tracePasses(Loaded &L, const PipelineConfig &C, const DiffOptions &O,
                 const std::string &SaveDir, std::vector<PassStep> &Steps,
                 PipelineResult &R, std::string &Err) {
  Module &M = *L.M;
  std::string Prev = printModule(M);
  if (!SaveDir.empty()) {
    if (auto EC = sys::fs::create_directories(SaveDir)) {
      Err = SaveDir + ": " + EC.message();
      return false;
    }
    std::error_code EC;
    raw_fd_ostream OS(SaveDir + "/0000-input.ll", EC);
    OS << Prev;
  }
  std::string StepErr;
  Runner Rn{M, C, R, {}, {}, 0};
  Rn.OnChange = [&](const PassRun &Run, const std::vector<std::string> &Funcs) {
    if (!StepErr.empty())
      return;
    if (!O.Functions.empty() && !Funcs.empty()) {
      bool Any = false;
      for (auto &F : Funcs)
        Any |= matchesAny(O.Functions, F);
      if (!Any)
        return;
    }
    std::string Cur = printModule(M);
    PassStep S;
    S.Run = Run;
    S.Functions = Funcs;
    if (!diffTexts(Prev, Cur, O, Funcs, S.Diff, StepErr))
      return;
    if (!SaveDir.empty()) {
      char Buf[16];
      snprintf(Buf, sizeof(Buf), "%04u", Run.Index);
      std::error_code EC;
      raw_fd_ostream OS(SaveDir + "/" + Buf + "-" + sanitize(Run.Pass) + "-" +
                            sanitize(Run.Unit) + ".ll",
                        EC);
      OS << Cur;
    }
    Prev = std::move(Cur);
    Steps.push_back(std::move(S));
  };
  installRemarkCollector(M.getContext(), R.Remarks,
                         [&Rn] { return Rn.current(); });
  if (!run(M, C, R, Err, &Rn))
    return false;
  if (!StepErr.empty()) {
    Err = "internal: could not re-read a pass snapshot: " + StepErr;
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
    if (H.consume_front("; "))
      ;
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
    if (!diffTexts(Prev, S.Text, O, {}, St.Diff, E)) {
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
