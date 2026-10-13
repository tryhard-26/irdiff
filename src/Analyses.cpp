#include "Analyses.h"
#include "Differ.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/DiagnosticHandler.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Remarks/Remark.h"
#include "llvm/Remarks/RemarkFormat.h"
#include "llvm/Remarks/RemarkParser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>

using namespace llvm;

namespace irdiff {

struct AnalysisHost::Impl {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  Impl() {
    FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  }
};

AnalysisHost::AnalysisHost(Module &) : P(std::make_unique<Impl>()) {}
AnalysisHost::~AnalysisHost() = default;
FunctionAnalysisManager &AnalysisHost::fam() { return P->FAM; }

static bool want(const Options &O, StringRef A) {
  if (O.Analyses.empty())
    return true;
  for (auto &X : O.Analyses)
    if (X == A)
      return true;
  return false;
}

static std::string str(const SCEV *S) {
  std::string R;
  raw_string_ostream OS(R);
  S->print(OS);
  return R;
}

static std::string aliasStr(AliasResult R) {
  std::string S;
  raw_string_ostream OS(S);
  OS << R;
  return S;
}

static Change obs(StringRef Subject, StringRef Kind = "observation") {
  Change C;
  C.Cat = "analysis";
  C.Kind = Kind.str();
  C.Significance = Sig::Minor;
  C.Subject = Subject.str();
  return C;
}

static std::vector<unsigned> memInsts(FuncInfo &F, unsigned Limit,
                                      bool &Truncated) {
  std::vector<unsigned> R;
  Truncated = false;
  for (unsigned I = 0; I < F.Insts.size(); ++I) {
    Instruction *In = F.Insts[I].I;
    if (F.Insts[I].Debug || !(isa<LoadInst>(In) || isa<StoreInst>(In)))
      continue;
    if (R.size() >= Limit) {
      Truncated = true;
      break;
    }
    R.push_back(I);
  }
  return R;
}

void diffAnalyses(FuncPair &P, FuncDiff &FD, const Options &O, AnalysisHost &HA,
                  AnalysisHost &HB) {
  FuncInfo &A = P.A, &B = P.B;
  Function &FA = *A.F, &FB = *B.F;
  auto &MA = HA.fam(), &MB = HB.fam();
  auto Add = [&](Change C) {
    C.Func = FD.NameA;
    FD.Changes.push_back(std::move(C));
  };
  auto TA = [&](unsigned I) { return instText(P.CA, A, I); };
  auto TB = [&](unsigned I) { return instText(P.CB, B, I); };

  if (want(O, "dom")) {
    auto &DA = MA.getResult<DominatorTreeAnalysis>(FA);
    auto &DB = MB.getResult<DominatorTreeAnalysis>(FB);
    for (unsigned X = 0; X < A.Blocks.size(); ++X) {
      int Y = A.Blocks[X].Match;
      if (Y < 0 || X == 0)
        continue;
      auto *NA = DA.getNode(A.Blocks[X].BB);
      auto *NB = DB.getNode(B.Blocks[Y].BB);
      if (!NA || !NB || !NA->getIDom() || !NB->getIDom())
        continue;
      unsigned IA = A.BlockIdx[NA->getIDom()->getBlock()];
      unsigned IB = B.BlockIdx[NB->getIDom()->getBlock()];
      if (A.Blocks[IA].Match == int(IB))
        continue;
      Change C = obs("immediate dominator of %" + blockName(A, X), "modified");
      C.Block = blockName(A, X);
      C.Before = "%" + blockName(A, IA);
      C.After = "%" + blockName(B, IB);
      C.Details.push_back("dominator tree: idom %" + blockName(A, IA) +
                          " -> %" + blockName(B, IB));
      Add(C);
    }
  }

  if (want(O, "scev") || want(O, "loops")) {
    auto &LA = MA.getResult<LoopAnalysis>(FA);
    auto &LB = MB.getResult<LoopAnalysis>(FB);
    auto &SA = MA.getResult<ScalarEvolutionAnalysis>(FA);
    auto &SB = MB.getResult<ScalarEvolutionAnalysis>(FB);
    for (Loop *L : LA.getLoopsInPreorder()) {
      int Y = A.Blocks[A.BlockIdx[L->getHeader()]].Match;
      if (Y < 0)
        continue;
      Loop *K = LB.getLoopFor(B.Blocks[Y].BB);
      if (!K || K->getHeader() != B.Blocks[Y].BB)
        continue;
      std::vector<std::string> D;
      unsigned TCA = SA.getSmallConstantTripCount(L);
      unsigned TCB = SB.getSmallConstantTripCount(K);
      if (TCA != TCB)
        D.push_back("constant trip count: " +
                    (TCA ? std::to_string(TCA) : std::string("unknown")) +
                    " -> " +
                    (TCB ? std::to_string(TCB) : std::string("unknown")));
      unsigned MXA = SA.getSmallConstantMaxTripCount(L);
      unsigned MXB = SB.getSmallConstantMaxTripCount(K);
      if (MXA != MXB)
        D.push_back("max trip count: " +
                    (MXA ? std::to_string(MXA) : std::string("unknown")) +
                    " -> " +
                    (MXB ? std::to_string(MXB) : std::string("unknown")));
      std::string BA = str(SA.getBackedgeTakenCount(L));
      std::string BB = str(SB.getBackedgeTakenCount(K));
      if (P.strictText(false, BA) != P.strictText(true, BB))
        D.push_back("backedge-taken count: " + P.CA.display(A, BA) + " -> " +
                    P.CB.display(B, BB));
      if (!D.empty()) {
        Change C = obs("scalar evolution for loop %" +
                           blockName(A, A.BlockIdx[L->getHeader()]),
                       "modified");
        C.Block = blockName(A, A.BlockIdx[L->getHeader()]);
        C.Details = D;
        C.Significance = Sig::Major;
        Add(C);
      }
    }
  }

  if (want(O, "aa")) {
    auto &AA = MA.getResult<AAManager>(FA);
    auto &AB = MB.getResult<AAManager>(FB);
    bool TruncA, TruncB;
    auto LA = memInsts(A, O.AALimit, TruncA);
    memInsts(B, O.AALimit, TruncB);
    unsigned Pairs = 0, Changed = 0;
    std::vector<std::string> D;
    for (unsigned I = 0; I < LA.size(); ++I)
      for (unsigned J = I + 1; J < LA.size(); ++J) {
        int MI = A.Insts[LA[I]].Match, MJ = A.Insts[LA[J]].Match;
        if (MI < 0 || MJ < 0)
          continue;
        Instruction *XI = A.Insts[LA[I]].I, *XJ = A.Insts[LA[J]].I;
        Instruction *YI = B.Insts[MI].I, *YJ = B.Insts[MJ].I;
        if (!(isa<LoadInst>(YI) || isa<StoreInst>(YI)) ||
            !(isa<LoadInst>(YJ) || isa<StoreInst>(YJ)))
          continue;
        if (isa<LoadInst>(XI) && isa<LoadInst>(XJ))
          continue;
        ++Pairs;
        AliasResult RA =
            AA.alias(MemoryLocation::get(XI), MemoryLocation::get(XJ));
        AliasResult RB =
            AB.alias(MemoryLocation::get(YI), MemoryLocation::get(YJ));
        if (RA == RB)
          continue;
        ++Changed;
        Change C =
            obs("alias: " + TA(LA[I]) + "  vs  " + TA(LA[J]), "modified");
        C.Block = blockName(A, A.Insts[LA[I]].Block);
        C.Before = aliasStr(RA);
        C.After = aliasStr(RB);
        C.Significance = Sig::Major;
        C.Details.push_back("alias result " + aliasStr(RA) + " -> " +
                            aliasStr(RB));
        C.Details.push_back("B: " + TB(MI) + "  vs  " + TB(MJ));
        Add(C);
      }
    if (TruncA || TruncB) {
      Change C = obs("alias analysis coverage");
      C.Details.push_back("only the first " + std::to_string(O.AALimit) +
                          " loads/stores were compared (--aa-limit); " +
                          std::to_string(Changed) + " of " +
                          std::to_string(Pairs) + " pairs changed");
      Add(C);
    }
  }

  if (want(O, "mssa")) {
    MemorySSA &SA = MA.getResult<MemorySSAAnalysis>(FA).getMSSA();
    MemorySSA &SB = MB.getResult<MemorySSAAnalysis>(FB).getMSSA();
    auto Desc = [&](bool SideB, MemorySSA &S, MemoryAccess *M,
                    std::string &Shown) -> std::string {
      FuncInfo &F = SideB ? B : A;
      if (S.isLiveOnEntryDef(M)) {
        Shown = "liveOnEntry";
        return Shown;
      }
      if (auto *Phi = dyn_cast<MemoryPhi>(M)) {
        unsigned Blk = F.BlockIdx[Phi->getBlock()];
        Shown = "MemoryPhi in %" + blockName(F, Blk);
        int Mt = F.Blocks[Blk].Match;
        return "phi@" + (SideB     ? std::to_string(Blk)
                         : Mt >= 0 ? std::to_string(Mt)
                                   : "a" + std::to_string(Blk));
      }
      auto *UD = cast<MemoryUseOrDef>(M);
      Instruction *MI = UD->getMemoryInst();
      if (!MI) {
        Shown = "?";
        return Shown;
      }
      unsigned Idx = F.InstIdx.lookup(MI);
      Shown = SideB ? TB(Idx) : TA(Idx);
      int Mt = F.Insts[Idx].Match;
      return "i@" + (SideB     ? std::to_string(Idx)
                     : Mt >= 0 ? std::to_string(Mt)
                               : "a" + std::to_string(Idx));
    };
    for (unsigned I = 0; I < A.Insts.size(); ++I) {
      auto *LdA = dyn_cast<LoadInst>(A.Insts[I].I);
      int M = A.Insts[I].Match;
      if (!LdA || M < 0 || !isa<LoadInst>(B.Insts[M].I))
        continue;
      MemoryAccess *CA = SA.getWalker()->getClobberingMemoryAccess(LdA);
      MemoryAccess *CB =
          SB.getWalker()->getClobberingMemoryAccess(B.Insts[M].I);
      std::string ShA, ShB;
      if (!CA || !CB || Desc(false, SA, CA, ShA) == Desc(true, SB, CB, ShB))
        continue;
      Change C = obs("clobbering access of " + TA(I), "modified");
      C.Block = blockName(A, A.Insts[I].Block);
      C.Before = ShA;
      C.After = ShB;
      C.Significance = Sig::Major;
      C.Details.push_back("MemorySSA clobber: " + ShA + " -> " + ShB);
      Add(C);
    }
  }
}

std::vector<FuncDiff> analyzeModule(Loaded &L, const Options &O) {
  std::vector<FuncDiff> R;
  ModuleCanon C(*L.M, O);
  AnalysisHost H(*L.M);
  auto &FAM = H.fam();
  for (Function &F : *L.M) {
    if (F.isDeclaration())
      continue;
    std::string Name = globalToken(F, C);
    if (!O.Functions.empty() && !matchesAny(O.Functions, Name))
      continue;
    FuncInfo &FI = C.func(F);
    FuncDiff FD;
    FD.NameA = Name;
    FD.Status = "analyzed";
    FD.Stats.BlocksA = F.size();
    for (auto &I : FI.Insts)
      FD.Stats.InstsA += !I.Debug;
    auto Add = [&](Change Ch) {
      Ch.Func = Name;
      FD.Changes.push_back(std::move(Ch));
    };
    auto T = [&](unsigned I) { return instText(C, FI, I); };
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    if (want(O, "dom")) {
      Change Ch = obs("dominator tree");
      unsigned Unreach = 0;
      for (unsigned B = 0; B < FI.Blocks.size(); ++B) {
        auto *N = DT.getNode(FI.Blocks[B].BB);
        if (!N) {
          ++Unreach;
          Ch.Details.push_back("%" + blockName(FI, B) + ": unreachable");
        } else if (N->getIDom()) {
          Ch.Details.push_back(
              "%" + blockName(FI, B) + ": idom %" +
              blockName(FI, FI.BlockIdx[N->getIDom()->getBlock()]) +
              ", depth " + std::to_string(N->getLevel()));
        }
      }
      Ch.After = std::to_string(FI.Blocks.size()) + " blocks, " +
                 std::to_string(Unreach) + " unreachable";
      Add(Ch);
    }
    if (want(O, "loops") || want(O, "scev")) {
      auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
      for (Loop *Lp : LI.getLoopsInPreorder()) {
        Change Ch = obs("loop %" + blockName(FI, FI.BlockIdx[Lp->getHeader()]));
        Ch.Block = blockName(FI, FI.BlockIdx[Lp->getHeader()]);
        SmallVector<BasicBlock *, 4> Exits, Latches;
        Lp->getExitBlocks(Exits);
        Lp->getLoopLatches(Latches);
        Ch.After = "depth " + std::to_string(Lp->getLoopDepth()) + ", " +
                   std::to_string(Lp->getNumBlocks()) + " blocks";
        Ch.Details.push_back("latches: " + std::to_string(Latches.size()) +
                             ", exit blocks: " + std::to_string(Exits.size()));
        Ch.Details.push_back(
            std::string("preheader: ") +
            (Lp->getLoopPreheader()
                 ? "%" + blockName(FI, FI.BlockIdx[Lp->getLoopPreheader()])
                 : "none") +
            ", simplify form: " + (Lp->isLoopSimplifyForm() ? "yes" : "no") +
            ", LCSSA: " + (Lp->isLCSSAForm(DT) ? "yes" : "no"));
        if (want(O, "scev")) {
          unsigned TC = SE.getSmallConstantTripCount(Lp);
          Ch.Details.push_back("trip count: " + (TC ? std::to_string(TC)
                                                    : std::string("unknown")));
          Ch.Details.push_back(
              "backedge-taken count: " +
              C.display(FI, str(SE.getBackedgeTakenCount(Lp))));
        }
        if (MDNode *ID = Lp->getLoopID())
          Ch.Details.push_back("loop metadata: " + C.mdText(ID));
        Add(Ch);
      }
    }
    if (want(O, "aa")) {
      auto &AA = FAM.getResult<AAManager>(F);
      bool Trunc;
      auto L2 = memInsts(FI, O.AALimit, Trunc);
      std::map<std::string, unsigned> Count;
      Change Ch = obs("alias analysis");
      for (unsigned I = 0; I < L2.size(); ++I)
        for (unsigned J = I + 1; J < L2.size(); ++J) {
          Instruction *X = FI.Insts[L2[I]].I, *Y = FI.Insts[L2[J]].I;
          if (isa<LoadInst>(X) && isa<LoadInst>(Y))
            continue;
          AliasResult AR =
              AA.alias(MemoryLocation::get(X), MemoryLocation::get(Y));
          ++Count[aliasStr(AR)];
          if (AR != AliasResult::NoAlias)
            Ch.Details.push_back(aliasStr(AR) + ": " + T(L2[I]) + "  vs  " +
                                 T(L2[J]));
        }
      std::string S;
      for (auto &[K, N] : Count)
        S += (S.empty() ? "" : ", ") + K + " " + std::to_string(N);
      Ch.After = S.empty() ? "no load/store pairs" : S;
      if (Trunc)
        Ch.Details.push_back("limited to the first " +
                             std::to_string(O.AALimit) +
                             " loads/stores (--aa-limit)");
      Add(Ch);
    }
    if (want(O, "mssa")) {
      MemorySSA &MS = FAM.getResult<MemorySSAAnalysis>(F).getMSSA();
      Change Ch = obs("memory SSA clobbers");
      unsigned N = 0;
      for (unsigned I = 0; I < FI.Insts.size(); ++I) {
        auto *Ld = dyn_cast<LoadInst>(FI.Insts[I].I);
        if (!Ld)
          continue;
        ++N;
        MemoryAccess *M = MS.getWalker()->getClobberingMemoryAccess(Ld);
        std::string D;
        if (MS.isLiveOnEntryDef(M))
          D = "liveOnEntry";
        else if (auto *Phi = dyn_cast<MemoryPhi>(M))
          D = "MemoryPhi in %" + blockName(FI, FI.BlockIdx[Phi->getBlock()]);
        else if (auto *UD = dyn_cast<MemoryUseOrDef>(M))
          D = UD->getMemoryInst() ? T(FI.InstIdx.lookup(UD->getMemoryInst()))
                                  : "?";
        Ch.Details.push_back(T(I) + "  <-  " + D);
      }
      Ch.After = std::to_string(N) + " loads";
      Add(Ch);
    }
    R.push_back(std::move(FD));
  }
  return R;
}

namespace {
class Collector : public DiagnosticHandler {
public:
  Collector(std::vector<Remark> &Out, std::function<std::string()> Pass)
      : Out(Out), Pass(std::move(Pass)) {}
  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    auto *R = dyn_cast<DiagnosticInfoOptimizationBase>(&DI);
    if (!R)
      return false;
    Remark M;
    switch (DI.getKind()) {
    case DK_OptimizationRemark:
      M.Type = "Passed";
      break;
    case DK_OptimizationRemarkMissed:
      M.Type = "Missed";
      break;
    case DK_OptimizationFailure:
      M.Type = "Failure";
      break;
    default:
      M.Type = "Analysis";
      break;
    }
    M.Pass = R->getPassName().str();
    M.Name = R->getRemarkName().str();
    if (auto *IR = dyn_cast<DiagnosticInfoIROptimization>(&DI))
      M.Function = IR->getFunction().getName().str();
    if (R->isLocationAvailable())
      M.Loc = R->getLocationStr();
    M.Message = R->getMsg();
    if (Pass)
      M.Origin = Pass();
    Out.push_back(std::move(M));
    return true;
  }
  bool isAnalysisRemarkEnabled(StringRef) const override { return true; }
  bool isMissedOptRemarkEnabled(StringRef) const override { return true; }
  bool isPassedOptRemarkEnabled(StringRef) const override { return true; }
  bool isAnyRemarkEnabled() const override { return true; }

private:
  std::vector<Remark> &Out;
  std::function<std::string()> Pass;
};
} // namespace

void installRemarkCollector(LLVMContext &Ctx, std::vector<Remark> &Out,
                            std::function<std::string()> CurrentPass) {
  Ctx.setDiagnosticHandler(
      std::make_unique<Collector>(Out, std::move(CurrentPass)));
}

bool loadRemarks(const std::string &Path, std::vector<Remark> &Out,
                 std::string &Err) {
  auto Buf = MemoryBuffer::getFileOrSTDIN(Path);
  if (!Buf) {
    Err = Path + ": cannot read: " + Buf.getError().message();
    return false;
  }
  StringRef Data = (*Buf)->getBuffer();
  remarks::Format Fmt = Data.starts_with("RMRK") ? remarks::Format::Bitstream
                                                 : remarks::Format::YAML;
  auto Parser = remarks::createRemarkParser(Fmt, Data);
  if (!Parser) {
    Err = Path + ": " + toString(Parser.takeError());
    return false;
  }
  while (true) {
    auto R = (*Parser)->next();
    if (!R) {
      Error E = R.takeError();
      if (E.isA<remarks::EndOfFileError>()) {
        consumeError(std::move(E));
        return true;
      }
      Err = Path + ": malformed remarks: " + toString(std::move(E));
      return false;
    }
    remarks::Remark &X = **R;
    Remark M;
    switch (X.RemarkType) {
    case remarks::Type::Passed:
      M.Type = "Passed";
      break;
    case remarks::Type::Missed:
      M.Type = "Missed";
      break;
    case remarks::Type::Failure:
      M.Type = "Failure";
      break;
    default:
      M.Type = "Analysis";
      break;
    }
    M.Pass = X.PassName.str();
    M.Name = X.RemarkName.str();
    M.Function = X.FunctionName.str();
    if (X.Loc)
      M.Loc = X.Loc->SourceFilePath.str() + ":" +
              std::to_string(X.Loc->SourceLine) + ":" +
              std::to_string(X.Loc->SourceColumn);
    M.Message = X.getArgsAsMsg();
    Out.push_back(std::move(M));
  }
}

void diffRemarks(const std::vector<Remark> &A, const std::vector<Remark> &B,
                 ModuleDiff &D, const Options &O) {
  using Key = std::tuple<std::string, std::string, std::string, std::string,
                         std::string>;
  auto Group = [&](const std::vector<Remark> &L) {
    std::map<Key, std::vector<const Remark *>> G;
    for (const Remark &R : L) {
      if (!O.Functions.empty() && !matchesAny(O.Functions, R.Function))
        continue;
      G[{R.Function, R.Pass, R.Name, R.Type, R.Loc}].push_back(&R);
    }
    for (auto &[K, V] : G)
      std::stable_sort(V.begin(), V.end(),
                       [](const Remark *X, const Remark *Y) {
                         return X->Message < Y->Message;
                       });
    return G;
  };
  auto GA = Group(A), GB = Group(B);
  auto Mk = [&](const Key &K, StringRef Kind) {
    Change C;
    C.Cat = "remark";
    C.Kind = Kind.str();
    C.Func = std::get<0>(K);
    C.Subject = "[" + std::get<3>(K) + "] " + std::get<1>(K) + "/" +
                std::get<2>(K) +
                (std::get<0>(K).empty() ? "" : " in @" + std::get<0>(K));
    C.Significance = std::get<3>(K) == "Analysis" ? Sig::Minor : Sig::Major;
    if (!std::get<4>(K).empty())
      C.Details.push_back("at " + std::get<4>(K));
    return C;
  };
  std::set<Key> Keys;
  for (auto &[K, V] : GA)
    Keys.insert(K);
  for (auto &[K, V] : GB)
    Keys.insert(K);
  for (const Key &K : Keys) {
    auto IA = GA.find(K), IB = GB.find(K);
    std::vector<std::string> MA, MB;
    std::string OA, OB;
    if (IA != GA.end())
      for (auto *R : IA->second) {
        MA.push_back(R->Message);
        if (OA.empty())
          OA = R->Origin;
      }
    if (IB != GB.end())
      for (auto *R : IB->second) {
        MB.push_back(R->Message);
        if (OB.empty())
          OB = R->Origin;
      }
    if (MA == MB)
      continue;
    if (MA.size() == 1 && MB.size() == 1) {
      Change C = Mk(K, "modified");
      C.Before = MA[0];
      C.After = MB[0];
      C.Pass = OB;
      D.RemarkChanges.push_back(C);
      continue;
    }
    std::multiset<std::string> SA(MA.begin(), MA.end()),
        SB(MB.begin(), MB.end());
    auto Emit = [&](const std::multiset<std::string> &X,
                    const std::multiset<std::string> &Y, StringRef Kind,
                    const std::string &Origin) {
      for (auto &M : std::set<std::string>(X.begin(), X.end())) {
        size_t NX = X.count(M), NY = Y.count(M);
        if (NX <= NY)
          continue;
        Change C = Mk(K, Kind);
        (Kind == "added" ? C.After : C.Before) = M;
        C.Pass = Origin;
        if (NX - NY > 1)
          C.Details.push_back("emitted " + std::to_string(NX - NY) +
                              " more time(s)");
        D.RemarkChanges.push_back(C);
      }
    };
    Emit(SA, SB, "removed", OA);
    Emit(SB, SA, "added", OB);
  }
}

void addHints(ModuleDiff &D) {
  struct Hint {
    std::string Title;
    std::string Why;
    std::vector<std::string> Evidence;
  };
  std::map<std::pair<std::string, std::string>, Hint> H;
  auto Note = [&](const std::string &Func, const std::string &Id,
                  const std::string &Title, const std::string &Why,
                  std::string Ev) {
    Hint &X = H[{Func, Id}];
    X.Title = Title;
    X.Why = Why;
    if (X.Evidence.size() < 8)
      X.Evidence.push_back(std::move(Ev));
    else if (X.Evidence.size() == 8)
      X.Evidence.push_back("...");
  };
  auto Scan = [&](const Change &C) {
    std::string Ev = C.Subject;
    if (!C.Details.empty())
      Ev += " (" + C.Details.front() + ")";
    if (C.Cat == "remark" && (C.Kind == "added" || C.Kind == "modified") &&
        (C.Subject.find("[Missed]") == 0 || C.Subject.find("[Failure]") == 0))
      Note(C.Func, "remark-missed", "LLVM reported a missed optimization in B",
           "This is LLVM's own remark text; it states what the pass "
           "observed, not necessarily the root cause.",
           C.Subject + ": " + C.After);
    if (C.Cat == "remark" && C.Kind == "removed" &&
        C.Subject.find("[Passed]") == 0)
      Note(C.Func, "remark-passed-lost",
           "An optimization reported as applied in A is not reported in B",
           "Compare the surrounding IR and analysis differences for the "
           "same location.",
           C.Subject + ": " + C.Before);
    if (C.Cat == "analysis" && C.Subject.rfind("alias:", 0) == 0 &&
        C.Before == "NoAlias" && C.After != "NoAlias")
      Note(C.Func, "aa-weaker", "Alias analysis is less precise in B",
           "Passes that rely on alias analysis (e.g. LICM, GVN, DSE, "
           "SLP/loop vectorization) may be affected; irdiff did not verify "
           "that they were.",
           Ev);
    if (C.Cat == "analysis" && C.Subject.rfind("scalar evolution", 0) == 0)
      for (auto &Dt : C.Details)
        if (Dt.find("-> unknown") != std::string::npos ||
            Dt.find("CouldNotCompute") != std::string::npos)
          Note(C.Func, "scev-unknown",
               "Loop trip count information is less precise in B",
               "Unrolling, vectorization and loop deletion consume trip "
               "counts; this may be relevant.",
               C.Subject + " (" + Dt + ")");
    if (C.Kind == "flags-changed" || C.Kind == "modified")
      for (auto &Dt : C.Details)
        if (Dt.rfind("flags:", 0) == 0 &&
            (Dt.find("-nsw") != std::string::npos ||
             Dt.find("-nuw") != std::string::npos ||
             Dt.find("-inbounds") != std::string::npos ||
             Dt.find("-exact") != std::string::npos ||
             Dt.find("-nusw") != std::string::npos))
          Note(C.Func, "flags-lost",
               "Poison-generating flags were dropped in B",
               "Missing nsw/nuw/inbounds can limit induction-variable and "
               "SCEV reasoning; this may be relevant.",
               Ev);
    if (C.Cat == "attribute")
      for (auto &Dt : C.Details)
        if (!Dt.empty() && Dt[0] == '-')
          Note(C.Func, "attrs-lost", "Function attributes were lost in B",
               "Attributes such as nounwind, willreturn, memory(...), "
               "noalias or nofree enable call-related optimizations.",
               C.Subject + ": " + Dt);
    if (C.Cat == "instruction" && C.Kind == "added" &&
        C.Subject.find("call ") != std::string::npos)
      Note(C.Func, "call-added", "Calls present in B that are absent in A",
           "If inlining was expected, check inliner remarks (pass 'inline') "
           "for these call sites.",
           C.Subject);
    if (C.Cat == "loop" && C.Kind == "removed")
      Note(C.Func, "loop-removed", "A loop in A has no counterpart in B",
           "This is consistent with full unrolling, loop deletion or "
           "restructuring; remarks or pass attribution can confirm which.",
           C.Subject);
    if (C.Cat == "metadata" &&
        (C.Before.find("!tbaa") != std::string::npos ||
         C.After.find("!tbaa") != std::string::npos ||
         (!C.Details.empty() &&
          C.Details[0].find("!tbaa") != std::string::npos)))
      Note(C.Func, "tbaa", "TBAA metadata differs",
           "Type-based alias analysis depends on !tbaa; missing or "
           "different tags may change alias results.",
           Ev);
    if (C.Cat == "pass")
      Note("", "pipeline", "The pass pipelines differ",
           "Pass presence and order determine which transformations can "
           "run; differences here may explain differing output.",
           Ev);
  };
  for (auto &C : D.ModuleChanges)
    Scan(C);
  for (auto &F : D.Funcs)
    for (auto &C : F.Changes)
      Scan(C);
  for (auto &C : D.RemarkChanges)
    Scan(C);
  for (auto &C : D.PassChanges)
    Scan(C);
  for (auto &[K, X] : H) {
    Change C;
    C.Cat = "hint";
    C.Kind = "observation";
    C.Significance = Sig::Major;
    C.Func = K.first;
    C.Subject = X.Title;
    C.Inferred = true;
    C.Details.push_back(X.Why);
    for (auto &E : X.Evidence)
      C.Details.push_back("evidence: " + E);
    D.ModuleChanges.push_back(C);
  }
}

} // namespace irdiff
