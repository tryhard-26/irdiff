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

using namespace llvm;

namespace irdiff {

using namespace detail;

static std::string sanitize(StringRef S) {
  std::string R;
  for (char C : S)
    R += (isalnum(static_cast<unsigned char>(C)) || C == '.' || C == '_' ||
          C == '-')
             ? C
             : '_';
  return R.substr(0, 80);
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

} // namespace irdiff
