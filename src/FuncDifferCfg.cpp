#include "Compat.h"
#include "DiffDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace irdiff::detail {

void FuncDiffer::cfg() {
  auto Reach = [](FuncInfo &F) {
    std::vector<bool> R(F.Blocks.size(), false);
    std::vector<unsigned> W;
    if (!F.Blocks.empty()) {
      R[0] = true;
      W.push_back(0);
    }
    while (!W.empty()) {
      unsigned X = W.back();
      W.pop_back();
      for (unsigned S : F.Blocks[X].Succ)
        if (!R[S]) {
          R[S] = true;
          W.push_back(S);
        }
    }
    return R;
  };
  auto RA = Reach(A), RB = Reach(B);
  std::set<std::pair<int, int>> EA, EB;
  unsigned NEA = 0, NEB = 0;
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb)
    for (unsigned S : A.Blocks[Xb].Succ) {
      ++NEA;
      if (A.Blocks[Xb].Match >= 0 && A.Blocks[S].Match >= 0)
        EA.insert({A.Blocks[Xb].Match, A.Blocks[S].Match});
    }
  for (unsigned Yb = 0; Yb < B.Blocks.size(); ++Yb)
    for (unsigned S : B.Blocks[Yb].Succ) {
      ++NEB;
      if (B.Blocks[Yb].Match >= 0 && B.Blocks[S].Match >= 0)
        EB.insert({int(Yb), int(S)});
    }
  for (auto &E : EA)
    if (!EB.count(E))
      add(mk("cfg", "removed", Sig::Major, "", bA(B.Blocks[E.first].Match),
             "edge %" + bA(B.Blocks[E.first].Match) + " -> %" +
                 bA(B.Blocks[E.second].Match)));
  for (auto &E : EB)
    if (!EA.count(E))
      add(mk("cfg", "added", Sig::Major, "", bB(E.first),
             "edge %" + bB(E.first) + " -> %" + bB(E.second)));

  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb) {
    int Yb = A.Blocks[Xb].Match;
    if (Yb < 0)
      continue;
    Instruction *TA = A.Blocks[Xb].BB->getTerminator();
    Instruction *TB = B.Blocks[Yb].BB->getTerminator();
    if (!TA || !TB)
      continue;
    if (TA->getOpcode() != TB->getOpcode() ||
        TA->getNumSuccessors() != TB->getNumSuccessors()) {
      Change C = mk("cfg", "modified", Sig::Major, "", bA(Xb),
                    "terminator of %" + bA(Xb), tA(A.Blocks[Xb].End - 1),
                    tB(B.Blocks[Yb].End - 1));
      C.Details.push_back(std::string("terminator: ") + TA->getOpcodeName() +
                          " (" + std::to_string(TA->getNumSuccessors()) +
                          " successors) -> " + TB->getOpcodeName() + " (" +
                          std::to_string(TB->getNumSuccessors()) +
                          " successors)");
      add(C);
      continue;
    }
    Value *CondA = branchCondition(TA), *CondB = branchCondition(TB);
    if (CondA && CondB) {
      std::string CA = X.CA.operand(CondA);
      std::string CB = X.CB.operand(CondB);
      auto Succ = [&](bool SideB, Instruction *Br, unsigned K) {
        return P.strictText(SideB,
                            (SideB ? X.CB : X.CA).operand(Br->getSuccessor(K)));
      };
      bool Same0 = Succ(false, TA, 0) == Succ(true, TB, 0);
      bool Swap = Succ(false, TA, 0) == Succ(true, TB, 1) &&
                  Succ(false, TA, 1) == Succ(true, TB, 0);
      bool CondSame = P.strictText(false, CA) == P.strictText(true, CB);
      if (!CondSame || (!Same0 && Swap)) {
        Change C = mk("cfg", "modified", Sig::Major, "", bA(Xb),
                      "branch condition of %" + bA(Xb), X.CA.display(A, CA),
                      X.CB.display(B, CB));
        if (!CondSame) {
          C.Details.push_back("condition: " + X.CA.display(A, CA) + " -> " +
                              X.CB.display(B, CB));
          if (auto *DI = dyn_cast<Instruction>(CondA))
            C.Details.push_back("A condition: " + tA(A.InstIdx.lookup(DI)));
          if (auto *DI = dyn_cast<Instruction>(CondB))
            C.Details.push_back("B condition: " + tB(B.InstIdx.lookup(DI)));
        }
        if (!Same0 && Swap) {
          C.Details.push_back("true/false successors swapped");
          auto *CmpA = dyn_cast<CmpInst>(CondA);
          auto *CmpB = dyn_cast<CmpInst>(CondB);
          if (CmpA && CmpB &&
              CmpA->getInversePredicate() == CmpB->getPredicate() &&
              A.Insts[A.InstIdx.lookup(CmpA)].Match ==
                  int(B.InstIdx.lookup(CmpB)) &&
              P.strictText(false, X.CA.operand(CmpA->getOperand(0))) ==
                  P.strictText(true, X.CB.operand(CmpB->getOperand(0))) &&
              P.strictText(false, X.CA.operand(CmpA->getOperand(1))) ==
                  P.strictText(true, X.CB.operand(CmpB->getOperand(1)))) {
            C.Details.push_back("condition predicate inverted together with "
                                "the successor swap: control flow is "
                                "equivalent");
            C.Significance = Sig::Minor;
          }
        }
        add(C);
      }
    }
  }
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb) {
    int Yb = A.Blocks[Xb].Match;
    bool UA = !RA[Xb], UB = Yb >= 0 && !RB[Yb];
    if (Yb >= 0 && UA != UB) {
      Change C = mk("cfg", "unreachable", Sig::Major, "", bA(Xb), "%" + bA(Xb));
      C.Details.push_back(UA ? "unreachable from entry in A, reachable in B"
                             : "reachable in A, unreachable from entry in B");
      add(C);
    } else if (Yb < 0 && UA) {
      Change C = mk("cfg", "unreachable", Sig::Minor, "", bA(Xb), "%" + bA(Xb));
      C.Details.push_back("removed block was unreachable from entry in A");
      add(C);
    }
  }
  for (unsigned Yb = 0; Yb < B.Blocks.size(); ++Yb)
    if (B.Blocks[Yb].Match < 0 && !RB[Yb]) {
      Change C = mk("cfg", "unreachable", Sig::Major, "", bB(Yb), "%" + bB(Yb));
      C.Details.push_back("added block is unreachable from entry in B");
      add(C);
    }

  auto Critical = [](FuncInfo &F) {
    unsigned N = 0;
    for (auto &BI : F.Blocks)
      if (BI.Succ.size() > 1)
        for (unsigned S : BI.Succ)
          N += F.Blocks[S].Pred.size() > 1;
    return N;
  };
  auto Rets = [](FuncInfo &F) {
    unsigned N = 0;
    for (auto &BI : F.Blocks)
      N += BI.BB->getTerminator() && isa<ReturnInst>(BI.BB->getTerminator());
    return N;
  };
  std::vector<std::string> T;
  auto Metric = [&](StringRef Name, long VA, long VB) {
    if (VA != VB)
      T.push_back(Name.str() + " " + std::to_string(VA) + " -> " +
                  std::to_string(VB));
  };
  Metric("blocks", A.Blocks.size(), B.Blocks.size());
  Metric("edges", NEA, NEB);
  Metric("cyclomatic complexity", long(NEA) - long(A.Blocks.size()) + 2,
         long(NEB) - long(B.Blocks.size()) + 2);
  Metric("critical edges", Critical(A), Critical(B));
  Metric("return blocks", Rets(A), Rets(B));
  if (!T.empty()) {
    Change C = mk("cfg", "modified", Sig::Major, "", "", "CFG topology");
    C.Details = T;
    add(C);
  }
}

void FuncDiffer::loops() {
  auto All = [](LoopInfo &LI) {
    std::vector<Loop *> R;
    for (Loop *L : LI.getLoopsInPreorder())
      R.push_back(L);
    return R;
  };
  auto LA = All(*LIA), LB = All(*LIB);
  std::map<Loop *, Loop *> M;
  std::set<Loop *> Used;
  for (Loop *L : LA) {
    int H = B.Blocks.empty() ? -1 : A.Blocks[A.BlockIdx[L->getHeader()]].Match;
    Loop *Cand = nullptr;
    if (H >= 0)
      Cand = LIB->getLoopFor(B.Blocks[H].BB);
    if (Cand && Cand->getHeader() != B.Blocks[H].BB)
      Cand = nullptr;
    if (!Cand) {
      std::map<Loop *, unsigned> Votes;
      for (BasicBlock *BB : L->blocks()) {
        int Y = A.Blocks[A.BlockIdx[BB]].Match;
        if (Y >= 0)
          if (Loop *LB2 = LIB->getLoopFor(B.Blocks[Y].BB))
            ++Votes[LB2];
      }
      unsigned Best = 0;
      for (auto &[Lp, N] : Votes)
        if (N > Best && !Used.count(Lp) &&
            Lp->getLoopDepth() == L->getLoopDepth()) {
          Best = N;
          Cand = Lp;
        }
      if (Best * 2 < L->getNumBlocks())
        Cand = nullptr;
    }
    if (Cand && !Used.count(Cand)) {
      M[L] = Cand;
      Used.insert(Cand);
    }
  }
  auto Desc = [&](bool SideB, Loop *L) {
    FuncInfo &F = SideB ? B : A;
    return "loop %" + blockName(F, F.BlockIdx[L->getHeader()]) + " (depth " +
           std::to_string(L->getLoopDepth()) + ", " +
           std::to_string(L->getNumBlocks()) + " blocks)";
  };
  auto Rotated = [](Loop *L) {
    BasicBlock *Latch = L->getLoopLatch();
    return Latch && L->isLoopExiting(Latch);
  };
  for (Loop *L : LA) {
    auto It = M.find(L);
    std::string HA = blockName(A, A.BlockIdx[L->getHeader()]);
    if (It == M.end()) {
      add(mk("loop", "removed", Sig::Major, "", HA, Desc(false, L)));
      continue;
    }
    Loop *K = It->second;
    std::vector<std::string> D;
    auto Cmp = [&](StringRef What, std::string VA, std::string VB) {
      if (VA != VB)
        D.push_back(What.str() + ": " + VA + " -> " + VB);
    };
    std::string HB = blockName(B, B.BlockIdx[K->getHeader()]);
    int HM = A.Blocks[A.BlockIdx[L->getHeader()]].Match;
    if (HM < 0 || B.Blocks[HM].BB != K->getHeader())
      D.push_back("header: %" + HA + " -> %" + HB);
    Cmp("depth", std::to_string(L->getLoopDepth()),
        std::to_string(K->getLoopDepth()));
    Cmp("blocks", std::to_string(L->getNumBlocks()),
        std::to_string(K->getNumBlocks()));
    SmallVector<BasicBlock *, 4> XA, XB, LtA, LtB;
    L->getExitBlocks(XA);
    K->getExitBlocks(XB);
    L->getLoopLatches(LtA);
    K->getLoopLatches(LtB);
    Cmp("exit blocks", std::to_string(XA.size()), std::to_string(XB.size()));
    Cmp("latches", std::to_string(LtA.size()), std::to_string(LtB.size()));
    Cmp("preheader", L->getLoopPreheader() ? "yes" : "no",
        K->getLoopPreheader() ? "yes" : "no");
    Cmp("rotated (latch exits)", Rotated(L) ? "yes" : "no",
        Rotated(K) ? "yes" : "no");
    Cmp("simplify form", L->isLoopSimplifyForm() ? "yes" : "no",
        K->isLoopSimplifyForm() ? "yes" : "no");
    Cmp("subloops", std::to_string(L->getSubLoops().size()),
        std::to_string(K->getSubLoops().size()));
    std::string MA = L->getLoopID() ? X.CA.mdText(L->getLoopID()) : "";
    std::string MB = K->getLoopID() ? X.CB.mdText(K->getLoopID()) : "";
    if (X.O.CompareMetadata)
      Cmp("loop metadata", MA.empty() ? "(none)" : MA,
          MB.empty() ? "(none)" : MB);
    if (!D.empty()) {
      Change C = mk("loop", "modified", Sig::Major, "", HA, Desc(false, L),
                    Desc(false, L), Desc(true, K));
      C.Details = D;
      add(C);
    }
  }
  for (Loop *L : LB)
    if (!Used.count(L))
      add(mk("loop", "added", Sig::Major, "",
             blockName(B, B.BlockIdx[L->getHeader()]), Desc(true, L)));
}

void FuncDiffer::debug() {
  if (A.DebugVars == B.DebugVars)
    return;
  Change C =
      mk("debug", "modified", Sig::Minor, "", "", "debug variable records");
  std::multiset<std::string> SA(A.DebugVars.begin(), A.DebugVars.end());
  std::multiset<std::string> SB(B.DebugVars.begin(), B.DebugVars.end());
  for (auto &V : std::set<std::string>(SA.begin(), SA.end()))
    if (SA.count(V) != SB.count(V))
      C.Details.push_back(V + ": " + std::to_string(SA.count(V)) + " -> " +
                          std::to_string(SB.count(V)));
  for (auto &V : std::set<std::string>(SB.begin(), SB.end()))
    if (!SA.count(V))
      C.Details.push_back(V + ": 0 -> " + std::to_string(SB.count(V)));
  add(C);
}

void FuncDiffer::rows() {
  bool Canon = X.O.NameMode == Names::Canonical;
  auto Txt = [&](bool SideB, unsigned I) {
    FuncInfo &F = SideB ? B : A;
    if (!Canon)
      return std::string("  ") + (SideB ? tB(I) : tA(I));
    return "  " +
           rewrite(stripFnAttrs(F.Insts[I].Canon),
                   [&](Tok T, unsigned Id, StringRef Raw, std::string &Out) {
                     const std::vector<int> &V = SideB ? P.VB : P.VA;
                     if (T == Tok::Local) {
                       if (V[Id] >= 0)
                         Out += "%v" + utostr(SideB ? Id : unsigned(V[Id]));
                       else
                         Out += (SideB ? "%b" : "%a") + utostr(Id);
                     } else if (T == Tok::Block) {
                       int M = F.Blocks[Id].Match;
                       if (M >= 0)
                         Out += "%bb" + utostr(SideB ? Id : unsigned(M));
                       else
                         Out += (SideB ? "%bbB" : "%bbA") + utostr(Id);
                     } else if (SideB) {
                       Out += P.mapGlobals(Raw);
                     } else {
                       Out += Raw;
                     }
                   });
  };
  auto Lbl = [&](bool SideB, unsigned Blk) {
    FuncInfo &F = SideB ? B : A;
    if (!Canon)
      return blockName(F, Blk) + ":";
    int M = F.Blocks[Blk].Match;
    if (M >= 0)
      return "bb" + utostr(SideB ? Blk : unsigned(M)) + ":";
    return std::string(SideB ? "bbB" : "bbA") + utostr(Blk) + ":";
  };
  auto Show = [&](const InstInfo &I) { return !I.Debug; };
  auto Push = [&](Row::Tag T, std::string L, std::string R,
                  std::string N = "") {
    Row Rw;
    Rw.T = T;
    Rw.Left = std::move(L);
    Rw.Right = std::move(R);
    Rw.Note = std::move(N);
    FD.Rows.push_back(std::move(Rw));
  };
  auto NoteA = [&](unsigned I) -> std::string {
    int M = A.Insts[I].Match;
    if (M < 0)
      return "";
    if (A.Blocks[A.Insts[I].Block].Match == int(B.Insts[M].Block))
      return "reordered";
    return "moved to %" + bB(B.Insts[M].Block);
  };
  auto NoteB = [&](unsigned I) -> std::string {
    int M = B.Insts[I].Match;
    if (M < 0)
      return "";
    if (B.Blocks[B.Insts[I].Block].Match == int(A.Insts[M].Block))
      return "reordered";
    return "moved from %" + bA(A.Insts[M].Block);
  };
  auto WholeA = [&](unsigned Xb, std::string Note) {
    Push(Row::Del, Lbl(false, Xb), "", Note);
    for (unsigned I = A.Blocks[Xb].First; I < A.Blocks[Xb].End; ++I)
      if (Show(A.Insts[I]))
        Push(Row::Del, Txt(false, I), "", NoteA(I));
  };
  auto WholeB = [&](unsigned Yb, std::string Note) {
    Push(Row::Add, "", Lbl(true, Yb), Note);
    for (unsigned I = B.Blocks[Yb].First; I < B.Blocks[Yb].End; ++I)
      if (Show(B.Insts[I]))
        Push(Row::Add, "", Txt(true, I), NoteB(I));
  };
  auto PairRows = [&](unsigned Xb, unsigned Yb, std::string Note) {
    Push(Row::Label, Lbl(false, Xb), Lbl(true, Yb), Note);
    const BlockInfo &BX = A.Blocks[Xb], &BY = B.Blocks[Yb];
    std::vector<std::pair<unsigned, unsigned>> Anchors;
    unsigned LastB = BY.First;
    for (unsigned I = BX.First; I < BX.End; ++I) {
      int M = A.Insts[I].Match;
      if (M >= int(BY.First) && M < int(BY.End) && unsigned(M) >= LastB) {
        Anchors.push_back({I, unsigned(M)});
        LastB = M + 1;
      }
    }
    unsigned IA = BX.First, IB = BY.First;
    for (auto [PA, PB] : Anchors) {
      for (; IA < PA; ++IA)
        if (Show(A.Insts[IA]))
          Push(Row::Del, Txt(false, IA), "", NoteA(IA));
      for (; IB < PB; ++IB)
        if (Show(B.Insts[IB]))
          Push(Row::Add, "", Txt(true, IB), NoteB(IB));
      std::string Why;
      bool Same = P.strictA(PA) == P.strictB(PB) || equivalent(PA, PB, Why);
      Push(Same ? Row::Same : Row::Mod, Txt(false, PA), Txt(true, PB), Why);
      IA = PA + 1;
      IB = PB + 1;
    }
    for (; IA < BX.End; ++IA)
      if (Show(A.Insts[IA]))
        Push(Row::Del, Txt(false, IA), "", NoteA(IA));
    for (; IB < BY.End; ++IB)
      if (Show(B.Insts[IB]))
        Push(Row::Add, "", Txt(true, IB), NoteB(IB));
  };

  std::vector<uint64_t> SA, SB;
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb)
    SA.push_back(Xb);
  for (unsigned Yb = 0; Yb < B.Blocks.size(); ++Yb)
    SB.push_back(B.Blocks[Yb].Match >= 0 ? uint64_t(B.Blocks[Yb].Match)
                                         : (uint64_t(1) << 40) + Yb);
  unsigned IA = 0, IB = 0;
  auto Flush = [&](unsigned EA, unsigned EB) {
    for (; IA < EA; ++IA)
      if (A.Blocks[IA].Match < 0)
        WholeA(IA, "removed block");
      else
        Push(Row::Del, Lbl(false, IA), "", "block moved");
    for (; IB < EB; ++IB)
      if (B.Blocks[IB].Match < 0)
        WholeB(IB, "added block");
      else
        PairRows(B.Blocks[IB].Match, IB, "block moved");
  };
  for (auto [Q, R] : lcs(SA, SB)) {
    Flush(Q, R);
    PairRows(Q, R, "");
    IA = Q + 1;
    IB = R + 1;
  }
  Flush(A.Blocks.size(), B.Blocks.size());
}

} // namespace irdiff::detail
