#include "Compat.h"
#include "DiffDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace irdiff::detail {

void FuncDiffer::run() {
  DTA = std::make_unique<DominatorTree>(*A.F);
  DTB = std::make_unique<DominatorTree>(*B.F);
  LIA = std::make_unique<LoopInfo>(*DTA);
  LIB = std::make_unique<LoopInfo>(*DTB);
  FuncStats &S = FD.Stats;
  S.BlocksA = A.Blocks.size();
  S.BlocksB = B.Blocks.size();
  for (auto &I : A.Insts)
    S.InstsA += !I.Debug;
  for (auto &I : B.Insts)
    S.InstsB += !I.Debug;
  header();
  instructions();
  blocks();
  cfg();
  loops();
  if (X.O.CompareDebug)
    debug();
  if (X.O.RunAnalyses && X.HA && X.HB)
    diffAnalyses(P, FD, X.O, *X.HA, *X.HB);
  std::vector<std::string> Amb;
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb)
    if (A.Blocks[Xb].Ambiguous && A.Blocks[Xb].Match >= 0)
      Amb.push_back("%" + bA(Xb) + " -> %" + bB(A.Blocks[Xb].Match) + " (" +
                    std::to_string(int(A.Blocks[Xb].Score * 100)) +
                    "% similar; another candidate scored within 5%)");
  if (!Amb.empty()) {
    Change C = mk("match", "ambiguous", Sig::Minor, "", "",
                  std::to_string(Amb.size()) +
                      " block match(es) have close alternatives");
    C.Confidence = "ambiguous";
    C.Details = Amb;
    FD.Ambiguous = true;
    add(C);
  }
  if (X.O.Rows)
    rows();
  Sig Max = Sig::Cosmetic;
  bool Any = false;
  for (auto &C : FD.Changes) {
    Any = true;
    Max = std::max(Max, C.Significance);
  }
  FD.Status = !Any                   ? "identical"
              : Max == Sig::Cosmetic ? "equivalent"
                                     : "modified";
}

void FuncDiffer::header() {
  auto HA = headerFields(*A.F, X.CA), HB = headerFields(*B.F, X.CB);
  for (unsigned I = 0; I < HA.size() && I < HB.size(); ++I) {
    const std::string &F = HA[I].first;
    std::string VA = HA[I].second, VB = P.mapGlobals(HB[I].second);
    if (VA == VB)
      continue;
    StringRef Cat = "function";
    Sig S = Sig::Minor;
    if (F == "linkage" || F == "visibility" || F == "dll-storage") {
      Cat = "linkage";
      S = Sig::Major;
    } else if (F == "dso-local" || F == "unnamed-addr" || F == "partition") {
      Cat = "linkage";
    } else if (F == "calling-convention" || F == "return-type" ||
               F == "parameters" || F == "address-space") {
      Cat = "signature";
      S = Sig::Major;
    } else if (F.find("attributes") != std::string::npos &&
               F != "config-attributes") {
      if (!X.O.CompareAttributes)
        continue;
      Cat = "attribute";
      S = Sig::Major;
    } else if (F == "config-attributes") {
      Cat = "config";
    } else if (F == "metadata") {
      Cat = "metadata";
    } else if (F == "personality") {
      S = Sig::Major;
    }
    Change C = mk(Cat, "modified", S, "", "", F, VA, HB[I].second);
    if (F == "parameter-attributes")
      C.Details = paramDelta(VA, VB);
    else if (Cat == "attribute" || Cat == "config")
      C.Details = setDelta(VA, VB);
    add(C);
  }
}

std::string FuncDiffer::canonText(bool SideB, unsigned I) {
  return stripFnAttrs(P.strictText(SideB, (SideB ? B : A).Insts[I].Canon));
}

// base + const byte offset + GEP flags of a const-index GEP chain
bool FuncDiffer::sameAddress(bool SideB, const Value *V, std::string &Key) {
  if (!V->getType()->isPointerTy())
    return false;
  const DataLayout &DL = (SideB ? B : A).F->getParent()->getDataLayout();
  APInt Off(DL.getIndexTypeSizeInBits(V->getType()), 0);
  bool Any = false, InBounds = true, NUSW = true, NUW = true;
  while (auto *G = dyn_cast<GEPOperator>(V)) {
    if (!G->accumulateConstantOffset(DL, Off))
      return false;
    GEPNoWrapFlags NW = G->getNoWrapFlags();
    InBounds &= NW.isInBounds();
    NUSW &= NW.hasNoUnsignedSignedWrap();
    NUW &= NW.hasNoUnsignedWrap();
    V = G->getPointerOperand();
    Any = true;
  }
  if (!Any)
    return false;
  Key = P.strictText(SideB, (SideB ? X.CB : X.CA).operand(V)) + " + " +
        toString(Off, 10, true) + (InBounds ? " inbounds" : "") +
        (NUSW ? " nusw" : "") + (NUW ? " nuw" : "");
  return true;
}

bool FuncDiffer::equivalent(unsigned IA, unsigned IB, std::string &Why,
                            std::string &Kind) {
  Instruction *IAp = A.Insts[IA].I, *IBp = B.Insts[IB].I;
  if (IAp->getOpcode() != IBp->getOpcode() ||
      P.strictText(false, A.Insts[IA].Md) !=
          P.strictText(true, B.Insts[IB].Md) ||
      fnAttrsOf(A.Insts[IA].Canon) != fnAttrsOf(B.Insts[IB].Canon))
    return false;
  auto Op = [&](bool SideB, const Value *V) {
    return P.strictText(SideB, (SideB ? X.CB : X.CA).operand(V));
  };
  if (auto *PA = dyn_cast<PHINode>(IAp)) {
    auto *PB = cast<PHINode>(IBp);
    if (typeStr(PA->getType()) != typeStr(PB->getType()))
      return false;
    std::vector<std::string> SA, SB;
    for (unsigned K = 0; K < PA->getNumIncomingValues(); ++K)
      SA.push_back(Op(false, PA->getIncomingBlock(K)) + "=" +
                   Op(false, PA->getIncomingValue(K)));
    for (unsigned K = 0; K < PB->getNumIncomingValues(); ++K)
      SB.push_back(Op(true, PB->getIncomingBlock(K)) + "=" +
                   Op(true, PB->getIncomingValue(K)));
    llvm::sort(SA);
    llvm::sort(SB);
    Why = "phi incoming order differs";
    return SA == SB;
  }
  if (auto *SA = dyn_cast<SwitchInst>(IAp)) {
    auto *SB = cast<SwitchInst>(IBp);
    if (Op(false, SA->getCondition()) != Op(true, SB->getCondition()) ||
        Op(false, SA->getDefaultDest()) != Op(true, SB->getDefaultDest()))
      return false;
    std::vector<std::string> CA, CB;
    for (auto &C : SA->cases())
      CA.push_back(Op(false, C.getCaseValue()) + "=" +
                   Op(false, C.getCaseSuccessor()));
    for (auto &C : SB->cases())
      CB.push_back(Op(true, C.getCaseValue()) + "=" +
                   Op(true, C.getCaseSuccessor()));
    llvm::sort(CA);
    llvm::sort(CB);
    Why = "switch case order differs";
    return CA == CB;
  }
  std::string KA, KB;
  if (isa<GetElementPtrInst>(IAp) &&
      typeStr(IAp->getType()) == typeStr(IBp->getType()) &&
      sameAddress(false, IAp, KA) && sameAddress(true, IBp, KB) && KA == KB) {
    Kind = "rewritten";
    Why = "same base, flags and constant byte offset in a different GEP form";
    return true;
  }
  if (IAp->getNumOperands() == IBp->getNumOperands() &&
      !isa<GetElementPtrInst>(IAp)) {
    std::string SA = P.strictA(IA), SB = P.strictB(IB);
    bool Any = false, Ok = SA != SB;
    for (unsigned K = 0; Ok && K < IAp->getNumOperands(); ++K) {
      Value *OA = IAp->getOperand(K), *OB = IBp->getOperand(K);
      if (Op(false, OA) == Op(true, OB))
        continue;
      std::string TA = P.strictText(false, X.CA.operand(OA, false)),
                  TB = P.strictText(true, X.CB.operand(OB, false));
      size_t At = SA.find(TA);
      Ok = isa<Constant>(OA) && isa<Constant>(OB) &&
           sameAddress(false, OA, KA) && sameAddress(true, OB, KB) &&
           KA == KB && At != std::string::npos;
      if (Ok)
        SA.replace(At, TA.size(), TB);
      Any = true;
    }
    if (Ok && Any && SA == SB) {
      Kind = "rewritten";
      Why = "constant GEP operand rewritten to the same base, flags and byte "
            "offset";
      return true;
    }
  }
  if (IAp->getNumOperands() != 2 || IBp->getNumOperands() != 2)
    return false;
  std::string A0 = Op(false, IAp->getOperand(0)),
              A1 = Op(false, IAp->getOperand(1));
  std::string B0 = Op(true, IBp->getOperand(0)),
              B1 = Op(true, IBp->getOperand(1));
  if (A0 != B1 || A1 != B0)
    return false;
  if (IAp->isCommutative()) {
    if (IAp->getRawSubclassOptionalData() !=
            IBp->getRawSubclassOptionalData() ||
        instFlags(*IAp) != instFlags(*IBp) ||
        typeStr(IAp->getType()) != typeStr(IBp->getType()))
      return false;
    Why = "commutative operands swapped";
    return true;
  }
  if (auto *CA = dyn_cast<CmpInst>(IAp)) {
    auto *CB = cast<CmpInst>(IBp);
    if (CA->getSwappedPredicate() != CB->getPredicate() ||
        IAp->getRawSubclassOptionalData() !=
            IBp->getRawSubclassOptionalData() ||
        instFlags(*IAp) != instFlags(*IBp))
      return false;
    Why = "operands swapped with mirrored predicate";
    return true;
  }
  return false;
}

void FuncDiffer::classify(unsigned IA, unsigned IB, bool Moved) {
  InstInfo &AI = A.Insts[IA], &BI = B.Insts[IB];
  std::string SA = P.strictA(IA), SB = P.strictB(IB);
  bool DbgDiff = X.O.CompareDebug && AI.DbgLoc != BI.DbgLoc;
  std::string Blk = bA(AI.Block);
  if (SA == SB) {
    if (Moved) {
      ++FD.Stats.Moved;
      Change C = mk("instruction", "moved", Sig::Major, "", Blk, tB(IB));
      unsigned DA = LIA->getLoopDepth(AI.I->getParent());
      unsigned DB = LIB->getLoopDepth(BI.I->getParent());
      C.Details.push_back("from %" + bA(AI.Block) + " (loop depth " +
                          std::to_string(DA) + ") to %" + bB(BI.Block) +
                          " (loop depth " + std::to_string(DB) + ")");
      if (DB < DA)
        C.Details.push_back("now outside " + std::to_string(DA - DB) +
                            " enclosing loop(s)");
      else if (DB > DA)
        C.Details.push_back("now inside " + std::to_string(DB - DA) +
                            " additional loop(s)");
      if (AI.How == HowPosition || AI.How == HowDefUseLoose)
        C.Confidence = "inferred-match";
      add(C);
    } else {
      ++FD.Stats.Unchanged;
    }
    if (DbgDiff) {
      Change C = mk("debug", "modified", Sig::Minor, "", Blk, tB(IB), AI.DbgLoc,
                    BI.DbgLoc);
      C.Details.push_back("debug location " + AI.DbgLoc + " -> " + BI.DbgLoc);
      add(C);
    }
    return;
  }
  std::string Why, Kind = "commuted";
  if (!Moved && equivalent(IA, IB, Why, Kind)) {
    ++FD.Stats.Unchanged;
    Change C =
        mk("instruction", Kind, Sig::Cosmetic, "", Blk, tA(IA), tA(IA), tB(IB));
    C.Details.push_back(Why + " (semantically equivalent)");
    add(C);
    return;
  }
  ++FD.Stats.Modified;
  Instruction *IAp = AI.I, *IBp = BI.I;
  Change C = mk("instruction", "modified", Sig::Major, "", Blk, tA(IA), tA(IA),
                tB(IB));
  if (Moved)
    C.Details.push_back("moved from %" + bA(AI.Block) + " to %" + bB(BI.Block));
  std::string CA = canonText(false, IA), CB = canonText(true, IB);
  bool MdOnly = CA == CB && fnAttrsOf(AI.Canon) == fnAttrsOf(BI.Canon);
  if (IAp->getOpcode() != IBp->getOpcode()) {
    C.Kind = "opcode-changed";
    C.Details.push_back(std::string("opcode: ") + IAp->getOpcodeName() +
                        " -> " + IBp->getOpcodeName());
  } else if (MdOnly) {
    C.Kind = "metadata-changed";
    C.Significance = Sig::Minor;
    C.Cat = "metadata";
    C.Details.push_back(
        "metadata: " +
        (AI.Md.empty() ? std::string("(none)") : X.CA.display(A, AI.Md)) +
        " -> " +
        (BI.Md.empty() ? std::string("(none)") : X.CB.display(B, BI.Md)));
  } else {
    std::vector<std::string> Kinds;
    if (typeStr(IAp->getType()) != typeStr(IBp->getType())) {
      C.Details.push_back("type: " + typeStr(IAp->getType()) + " -> " +
                          typeStr(IBp->getType()));
      Kinds.push_back("type-changed");
    }
    if (auto *PA = dyn_cast<CmpInst>(IAp))
      if (PA->getPredicate() != cast<CmpInst>(IBp)->getPredicate()) {
        C.Details.push_back(
            "predicate: " +
            CmpInst::getPredicateName(PA->getPredicate()).str() + " -> " +
            CmpInst::getPredicateName(cast<CmpInst>(IBp)->getPredicate())
                .str());
        Kinds.push_back("modified");
      }
    auto WA = instFlags(*IAp), WB = instFlags(*IBp);
    if (WA != WB) {
      std::string F;
      for (auto &W : WA)
        if (WB.count(W) < WA.count(W) && F.find("-" + W) == std::string::npos)
          F += (F.empty() ? "" : " ") + ("-" + W);
      for (auto &W : WB)
        if (WA.count(W) < WB.count(W) && F.find("+" + W) == std::string::npos)
          F += (F.empty() ? "" : " ") + ("+" + W);
      C.Details.push_back("flags: " + F);
      Kinds.push_back("flags-changed");
    }
    std::string AlA = alignOf(CA), AlB = alignOf(CB);
    if (AlA != AlB) {
      C.Details.push_back("align: " + (AlA.empty() ? "-" : AlA) + " -> " +
                          (AlB.empty() ? "-" : AlB));
      Kinds.push_back("modified");
    }
    std::string FA = fnAttrsOf(AI.Canon), FB = fnAttrsOf(BI.Canon);
    if (FA != FB) {
      std::string D;
      for (auto &T : setDelta(FA, FB))
        D += (D.empty() ? "" : " ") + T;
      C.Details.push_back("call attributes: " + D);
      Kinds.push_back("modified");
    }
    if (IAp->getNumOperands() == IBp->getNumOperands() && !isa<PHINode>(IAp)) {
      unsigned NOps = 0;
      for (unsigned K = 0; K < IAp->getNumOperands(); ++K) {
        Value *OA = IAp->getOperand(K), *OB = IBp->getOperand(K);
        std::string PA = X.CA.operand(OA), PB = X.CB.operand(OB);
        if (P.strictText(false, PA) == P.strictText(true, PB))
          continue;
        ++NOps;
        bool Callee = isa<CallBase>(IAp) && K + 1 == IAp->getNumOperands();
        std::string DA = X.CA.display(A, PA), DB = X.CB.display(B, PB);
        C.Details.push_back(
            (Callee ? std::string("callee") : "operand " + std::to_string(K)) +
            ": " + DA + " -> " + DB +
            (DA == DB ? " (same name, different matched entity)" : ""));
      }
      if (NOps)
        Kinds.push_back("operands-changed");
    } else if (auto *PA = dyn_cast<PHINode>(IAp)) {
      auto *PB = cast<PHINode>(IBp);
      for (unsigned K = 0; K < PA->getNumIncomingValues(); ++K) {
        unsigned BlkA = A.BlockIdx.lookup(PA->getIncomingBlock(K));
        int BlkB = A.Blocks[BlkA].Match;
        Value *VA = PA->getIncomingValue(K);
        if (BlkB < 0 || PB->getBasicBlockIndex(B.Blocks[BlkB].BB) < 0) {
          C.Details.push_back("incoming from %" + bA(BlkA) + " removed");
          continue;
        }
        Value *VB = PB->getIncomingValueForBlock(B.Blocks[BlkB].BB);
        std::string SVA = X.CA.operand(VA), SVB = X.CB.operand(VB);
        if (P.strictText(false, SVA) != P.strictText(true, SVB))
          C.Details.push_back("incoming from %" + bA(BlkA) + ": " +
                              X.CA.display(A, SVA) + " -> " +
                              X.CB.display(B, SVB));
      }
      for (unsigned K = 0; K < PB->getNumIncomingValues(); ++K) {
        unsigned BlkB = B.BlockIdx.lookup(PB->getIncomingBlock(K));
        int BlkA = B.Blocks[BlkB].Match;
        if (BlkA < 0 || PA->getBasicBlockIndex(A.Blocks[BlkA].BB) < 0)
          C.Details.push_back("incoming from %" + bB(BlkB) + " added");
      }
      Kinds.push_back("operands-changed");
    } else {
      C.Details.push_back(
          "operand count: " + std::to_string(IAp->getNumOperands()) + " -> " +
          std::to_string(IBp->getNumOperands()));
      Kinds.push_back("operands-changed");
    }
    if (P.strictText(false, AI.Md) != P.strictText(true, BI.Md))
      C.Details.push_back(
          "metadata: " +
          (AI.Md.empty() ? std::string("(none)") : X.CA.display(A, AI.Md)) +
          " -> " +
          (BI.Md.empty() ? std::string("(none)") : X.CB.display(B, BI.Md)));
    std::sort(Kinds.begin(), Kinds.end());
    Kinds.erase(std::unique(Kinds.begin(), Kinds.end()), Kinds.end());
    C.Kind = Kinds.size() == 1 ? Kinds[0] : "modified";
  }
  if (AI.How == HowPosition)
    C.Confidence = "positional-match";
  else if (AI.How == HowDefUseLoose)
    C.Confidence = "inferred-match";
  if (DbgDiff)
    C.Details.push_back("debug location " + AI.DbgLoc + " -> " + BI.DbgLoc);
  add(C);
}

static std::string rhs(const std::string &S) {
  size_t Eq = S.find(" = ");
  return !S.empty() && S[0] == '%' && Eq != std::string::npos ? S.substr(Eq + 3)
                                                              : S;
}

void FuncDiffer::instructions() {
  std::map<std::string, unsigned> SameA, SameB;
  for (unsigned I = 0; I < A.Insts.size(); ++I)
    if (!A.Insts[I].Debug && A.Insts[I].Match >= 0 &&
        !A.Insts[I].I->getType()->isVoidTy())
      SameA.emplace(rhs(P.strictA(I)), I);
  for (unsigned I = 0; I < B.Insts.size(); ++I)
    if (!B.Insts[I].Debug && B.Insts[I].Match >= 0 &&
        !B.Insts[I].I->getType()->isVoidTy())
      SameB.emplace(rhs(P.strictB(I)), I);
  for (unsigned I = 0; I < A.Insts.size(); ++I) {
    InstInfo &AI = A.Insts[I];
    if (AI.Debug)
      continue;
    if (AI.Match < 0) {
      ++FD.Stats.Removed;
      Change C =
          mk("instruction", "removed", Sig::Major, "", bA(AI.Block), tA(I));
      auto It = AI.I->getType()->isVoidTy() ? SameA.end()
                                            : SameA.find(rhs(P.strictA(I)));
      if (It != SameA.end()) {
        unsigned Keep = It->second;
        Instruction *KeepB = B.Insts[A.Insts[Keep].Match].I;
        unsigned Users = 0, Redirected = 0;
        for (const Use &U : AI.I->uses())
          if (auto *UI = dyn_cast<Instruction>(U.getUser())) {
            ++Users;
            int M = A.Insts[A.InstIdx.lookup(UI)].Match;
            Instruction *UB = M >= 0 ? B.Insts[M].I : nullptr;
            Redirected += UB && U.getOperandNo() < UB->getNumOperands() &&
                          UB->getOperand(U.getOperandNo()) == KeepB;
          }
        C.Kind = "merged";
        C.Inferred = true;
        C.Details.push_back("computed the same value as " + tA(Keep) +
                            ", which remains");
        if (Users)
          C.Details.push_back(std::to_string(Redirected) + " of " +
                              std::to_string(Users) +
                              " use(s) now read the remaining instruction");
      }
      add(C);
      continue;
    }
    bool Moved = A.Blocks[AI.Block].Match != int(B.Insts[AI.Match].Block);
    classify(I, AI.Match, Moved);
  }
  for (unsigned I = 0; I < B.Insts.size(); ++I)
    if (!B.Insts[I].Debug && B.Insts[I].Match < 0) {
      ++FD.Stats.Added;
      Change C = mk("instruction", "added", Sig::Major, "",
                    bB(B.Insts[I].Block), tB(I));
      auto It = B.Insts[I].I->getType()->isVoidTy()
                    ? SameB.end()
                    : SameB.find(rhs(P.strictB(I)));
      if (It != SameB.end()) {
        C.Kind = "split";
        C.Inferred = true;
        C.Details.push_back("duplicates " + tB(It->second) + " in %" +
                            bB(B.Insts[It->second].Block));
      }
      add(C);
    }
  // reorder in matched blk pairs = matches outside LIS
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb) {
    int Yb = A.Blocks[Xb].Match;
    if (Yb < 0)
      continue;
    std::vector<unsigned> Seq, Src;
    for (unsigned I = A.Blocks[Xb].First; I < A.Blocks[Xb].End; ++I) {
      int M = A.Insts[I].Match;
      if (!A.Insts[I].Debug && M >= 0 && B.Insts[M].Block == unsigned(Yb)) {
        Seq.push_back(M);
        Src.push_back(I);
      }
    }
    std::vector<uint64_t> KA(Seq.begin(), Seq.end()),
        KB(Seq.begin(), Seq.end());
    llvm::sort(KB);
    std::vector<bool> InLis(Seq.size(), false);
    for (auto [Q, R] : lcs(KA, KB))
      InLis[Q] = true;
    for (unsigned K = 0; K < Seq.size(); ++K)
      if (!InLis[K]) {
        unsigned I = Src[K];
        bool Pure = A.Insts[I].Pure && B.Insts[Seq[K]].Pure;
        Change C =
            mk("instruction", "reordered", Pure ? Sig::Cosmetic : Sig::Major,
               "", bA(Xb), tB(Seq[K]));
        C.Details.push_back(Pure ? "position changed within block; no side "
                                   "effects or memory access"
                                 : "position changed within block relative to "
                                   "other instructions (touches memory or has "
                                   "side effects)");
        add(C);
      }
  }
}

void FuncDiffer::blocks() {
  auto Votes = [&](bool SideB, unsigned Blk, unsigned &Matched) {
    FuncInfo &F = SideB ? B : A;
    FuncInfo &O = SideB ? A : B;
    std::map<unsigned, unsigned> V;
    Matched = 0;
    for (unsigned I = F.Blocks[Blk].First; I < F.Blocks[Blk].End; ++I) {
      if (F.Insts[I].Debug || F.Insts[I].I->isTerminator())
        continue;
      if (F.Insts[I].Match >= 0) {
        ++Matched;
        ++V[O.Insts[F.Insts[I].Match].Block];
      }
    }
    return V;
  };
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb) {
    if (A.Blocks[Xb].Match >= 0)
      continue;
    unsigned Matched;
    auto V = Votes(false, Xb, Matched);
    unsigned NonTerm = 0;
    for (unsigned I = A.Blocks[Xb].First; I < A.Blocks[Xb].End; ++I)
      NonTerm += !A.Insts[I].Debug && !A.Insts[I].I->isTerminator();
    Change C = mk("block", "removed", Sig::Major, "", bA(Xb), "%" + bA(Xb));
    if (NonTerm == 0) {
      C.Details.push_back("block contained only a terminator: " +
                          tA(A.Blocks[Xb].End - 1));
    } else if (!V.empty()) {
      auto Best = std::max_element(V.begin(), V.end(), [](auto &L, auto &R) {
        return L.second < R.second;
      });
      if (Best->second * 2 >= NonTerm) {
        C.Kind = "merged";
        C.Inferred = true;
        C.Subject = "%" + bA(Xb) + " -> %" + bB(Best->first);
        C.Details.push_back(
            std::to_string(Best->second) + " of " + std::to_string(NonTerm) +
            " non-terminator instructions now in %" + bB(Best->first));
        int Partner = B.Blocks[Best->first].Match;
        if (Partner >= 0 && bA(Partner) != bB(Best->first))
          C.Details.push_back("%" + bB(Best->first) + " corresponds to %" +
                              bA(Partner) + " in A");
      }
    }
    add(C);
  }
  for (unsigned Yb = 0; Yb < B.Blocks.size(); ++Yb) {
    if (B.Blocks[Yb].Match >= 0)
      continue;
    unsigned Matched;
    auto V = Votes(true, Yb, Matched);
    unsigned NonTerm = 0;
    for (unsigned I = B.Blocks[Yb].First; I < B.Blocks[Yb].End; ++I)
      NonTerm += !B.Insts[I].Debug && !B.Insts[I].I->isTerminator();
    Change C = mk("block", "added", Sig::Major, "", bB(Yb), "%" + bB(Yb));
    if (NonTerm == 0)
      C.Details.push_back("block contains only a terminator: " +
                          tB(B.Blocks[Yb].End - 1));
    else if (!V.empty()) {
      auto Best = std::max_element(V.begin(), V.end(), [](auto &L, auto &R) {
        return L.second < R.second;
      });
      if (Best->second * 2 >= NonTerm) {
        C.Kind = "split";
        C.Inferred = true;
        C.Subject = "%" + bA(Best->first) + " -> %" + bB(Yb);
        C.Details.push_back(
            std::to_string(Best->second) + " of " + std::to_string(NonTerm) +
            " non-terminator instructions came from %" + bA(Best->first));
      }
    }
    add(C);
  }
  std::vector<uint64_t> SA, SB;
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb)
    if (A.Blocks[Xb].Match >= 0)
      SA.push_back(Xb);
  for (unsigned Yb = 0; Yb < B.Blocks.size(); ++Yb)
    if (B.Blocks[Yb].Match >= 0)
      SB.push_back(B.Blocks[Yb].Match);
  std::set<uint64_t> InOrder;
  for (auto [Q, R] : lcs(SA, SB))
    InOrder.insert(SA[Q]);
  for (uint64_t Xb : SA)
    if (!InOrder.count(Xb)) {
      Change C =
          mk("block", "moved", X.O.IgnoreLayout ? Sig::Cosmetic : Sig::Minor,
             "", bA(Xb), "%" + bA(Xb));
      C.Details.push_back("layout position changed (no effect on semantics)");
      add(C);
    }
  for (unsigned Xb = 0; Xb < A.Blocks.size(); ++Xb) {
    int Yb = A.Blocks[Xb].Match;
    if (Yb >= 0 && A.Blocks[Xb].Label != B.Blocks[Yb].Label && !X.O.Strict) {
      Change C = mk("block", "renamed", Sig::Cosmetic, "", bA(Xb),
                    "%" + bA(Xb) + " -> %" + bB(Yb));
      add(C);
    }
  }
}

} // namespace irdiff::detail
