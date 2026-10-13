#include "Differ.h"
#include "Analyses.h"
#include "Compat.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

using namespace llvm;

namespace irdiff {

namespace {

Change mk(StringRef Cat, StringRef Kind, Sig S, StringRef Func, StringRef Block,
          std::string Subject, std::string Before = "",
          std::string After = "") {
  Change C;
  C.Cat = Cat.str();
  C.Kind = Kind.str();
  C.Significance = S;
  C.Func = Func.str();
  C.Block = Block.str();
  C.Subject = std::move(Subject);
  C.Before = std::move(Before);
  C.After = std::move(After);
  return C;
}

std::string typeStr(Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
  return S;
}

// split attrs on top-level spaces
std::vector<std::string> attrTokens(StringRef S) {
  std::vector<std::string> R;
  int Depth = 0;
  bool Q = false;
  std::string Cur;
  for (char C : S) {
    if (C == '"')
      Q = !Q;
    if (!Q && C == '(')
      ++Depth;
    if (!Q && C == ')')
      --Depth;
    if (C == ' ' && !Q && Depth == 0) {
      if (!Cur.empty())
        R.push_back(Cur);
      Cur.clear();
      continue;
    }
    Cur += C;
  }
  if (!Cur.empty())
    R.push_back(Cur);
  return R;
}

std::vector<std::string> setDelta(StringRef A, StringRef B) {
  auto TA = attrTokens(A), TB = attrTokens(B);
  std::set<std::string> SA(TA.begin(), TA.end()), SB(TB.begin(), TB.end());
  std::vector<std::string> R;
  for (auto &T : SA)
    if (!SB.count(T))
      R.push_back("-" + T);
  for (auto &T : SB)
    if (!SA.count(T))
      R.push_back("+" + T);
  return R;
}

const std::set<std::string> &flagWords() {
  static const std::set<std::string> W = {
      "nsw",      "nuw",      "exact",    "disjoint", "nneg",      "samesign",
      "inbounds", "nusw",     "inrange",  "fast",     "nnan",      "ninf",
      "nsz",      "arcp",     "contract", "afn",      "reassoc",   "volatile",
      "tail",     "musttail", "notail",   "atomic",   "unordered", "monotonic",
      "acquire",  "release",  "acq_rel",  "seq_cst",  "weak",      "syncscope"};
  return W;
}

// per-param delta of "#i: attrs; ..." strs
std::vector<std::string> paramDelta(StringRef A, StringRef B) {
  auto Split = [](StringRef S) {
    std::map<std::string, std::string> M;
    SmallVector<StringRef, 8> Parts;
    S.split(Parts, "; ", -1, false);
    for (StringRef P : Parts) {
      auto [K, V] = P.split(": ");
      M[K.str()] = V.str();
    }
    return M;
  };
  auto MA = Split(A), MB = Split(B);
  std::set<std::string> Keys;
  for (auto &[K, V] : MA)
    Keys.insert(K);
  for (auto &[K, V] : MB)
    Keys.insert(K);
  std::vector<std::string> R;
  for (auto &K : Keys)
    for (auto &D : setDelta(MA[K], MB[K]))
      R.push_back(D + " (param " + K + ")");
  return R;
}

std::multiset<std::string> words(StringRef S) {
  std::multiset<std::string> R;
  SmallVector<StringRef, 16> Parts;
  S.split(Parts, ' ', -1, false);
  for (StringRef P : Parts) {
    P = P.trim(",()");
    if (flagWords().count(P.str()))
      R.insert(P.str());
  }
  return R;
}

std::string alignOf(StringRef S) {
  size_t P = S.find(", align ");
  if (P == StringRef::npos)
    return "";
  StringRef R = S.substr(P + 8);
  return R.take_while(isDigit).str();
}

std::string fnAttrsOf(StringRef S) {
  size_t P = S.find(" [fn-attrs: ");
  if (P == StringRef::npos)
    return "";
  return S.substr(P + 12).drop_back().str();
}

std::string stripFnAttrs(StringRef S) {
  size_t P = S.find(" [fn-attrs: ");
  return (P == StringRef::npos ? S : S.substr(0, P)).str();
}

struct Ctx {
  ModuleCanon &CA, &CB;
  const GlobalMatch &GM;
  const DiffOptions &O;
  AnalysisHost *HA = nullptr, *HB = nullptr;
};

class FuncDiffer {
public:
  FuncDiffer(Ctx &X, FuncPair &P, FuncDiff &FD)
      : X(X), P(P), FD(FD), A(P.A), B(P.B) {}
  void run();

private:
  Ctx &X;
  FuncPair &P;
  FuncDiff &FD;
  FuncInfo &A, &B;
  std::unique_ptr<DominatorTree> DTA, DTB;
  std::unique_ptr<LoopInfo> LIA, LIB;

  std::string tA(unsigned I) { return instText(X.CA, A, I); }
  std::string tB(unsigned I) { return instText(X.CB, B, I); }
  std::string bA(unsigned Blk) { return blockName(A, Blk); }
  std::string bB(unsigned Blk) { return blockName(B, Blk); }
  void add(Change C) {
    if (C.Func.empty())
      C.Func = FD.NameA.empty() ? FD.NameB : FD.NameA;
    FD.Changes.push_back(std::move(C));
  }
  void header();
  void instructions();
  bool equivalent(unsigned IA, unsigned IB, std::string &Why);
  void classify(unsigned IA, unsigned IB, bool Moved);
  void blocks();
  void cfg();
  void loops();
  void debug();
  void rows();
  std::string canonText(bool SideB, unsigned I);
};

} // namespace

std::string blockName(const FuncInfo &F, unsigned Blk) {
  StringRef L = F.Blocks[Blk].Label;
  return L.starts_with("%") ? L.drop_front().str() : L.str();
}

std::string instText(ModuleCanon &C, const FuncInfo &F, unsigned I) {
  return C.display(F, stripFnAttrs(F.Insts[I].Canon));
}

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

bool FuncDiffer::equivalent(unsigned IA, unsigned IB, std::string &Why) {
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
        typeStr(IAp->getType()) != typeStr(IBp->getType()))
      return false;
    Why = "commutative operands swapped";
    return true;
  }
  if (auto *CA = dyn_cast<CmpInst>(IAp)) {
    auto *CB = cast<CmpInst>(IBp);
    if (CA->getSwappedPredicate() != CB->getPredicate() ||
        IAp->getRawSubclassOptionalData() != IBp->getRawSubclassOptionalData())
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
  std::string Why;
  if (!Moved && equivalent(IA, IB, Why)) {
    ++FD.Stats.Unchanged;
    Change C = mk("instruction", "commuted", Sig::Cosmetic, "", Blk, tA(IA),
                  tA(IA), tB(IB));
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
    auto WA = words(CA), WB = words(CB);
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

static void wholeFunctionRows(ModuleCanon &C, FuncInfo &F, bool SideB,
                              FuncDiff &FD) {
  for (unsigned Blk = 0; Blk < F.Blocks.size(); ++Blk) {
    Row L;
    L.T = SideB ? Row::Add : Row::Del;
    (SideB ? L.Right : L.Left) = blockName(F, Blk) + ":";
    FD.Rows.push_back(L);
    for (unsigned I = F.Blocks[Blk].First; I < F.Blocks[Blk].End; ++I) {
      if (F.Insts[I].Debug)
        continue;
      Row R;
      R.T = SideB ? Row::Add : Row::Del;
      (SideB ? R.Right : R.Left) = "  " + instText(C, F, I);
      FD.Rows.push_back(R);
    }
  }
}

static std::string gname(ModuleCanon &C, GlobalValue &G) {
  return globalToken(G, C);
}

static void moduleLevel(ModuleDiff &D, Ctx &X) {
  Module &MA = X.CA.M, &MB = X.CB.M;
  auto Add = [&](Change C) { D.ModuleChanges.push_back(std::move(C)); };
  if (tripleOf(MA) != tripleOf(MB))
    Add(mk("module", "modified", Sig::Major, "", "", "target triple",
           tripleOf(MA), tripleOf(MB)));
  if (MA.getDataLayoutStr() != MB.getDataLayoutStr())
    Add(mk("module", "modified", Sig::Major, "", "", "data layout",
           MA.getDataLayoutStr(), MB.getDataLayoutStr()));
  if (MA.getSourceFileName() != MB.getSourceFileName())
    Add(mk("module", "modified", X.O.Strict ? Sig::Minor : Sig::Cosmetic, "",
           "", "source_filename", MA.getSourceFileName(),
           MB.getSourceFileName()));
  if (moduleAsm(MA) != moduleAsm(MB))
    Add(mk("module", "modified", Sig::Major, "", "", "module asm",
           moduleAsm(MA), moduleAsm(MB)));

  auto Flags = [&](Module &M, ModuleCanon &C) {
    std::map<std::string, std::string> R;
    SmallVector<Module::ModuleFlagEntry, 8> E;
    M.getModuleFlagsMetadata(E);
    for (auto &F : E)
      R[F.Key->getString().str()] = C.mdText(F.Val);
    return R;
  };
  auto FA = Flags(MA, X.CA), FB = Flags(MB, X.CB);
  auto FlagCat = [](StringRef K) {
    return K.contains("Debug") || K.contains("Dwarf") || K.contains("CodeView")
               ? "debug"
               : "config";
  };
  for (auto &[K, V] : FA) {
    if (StringRef(FlagCat(K)) == "debug" && !X.O.CompareDebug)
      continue;
    auto It = FB.find(K);
    if (It == FB.end())
      Add(mk(FlagCat(K), "removed", Sig::Minor, "", "",
             "module flag \"" + K + "\"", V));
    else if (It->second != V)
      Add(mk(FlagCat(K), "modified", Sig::Minor, "", "",
             "module flag \"" + K + "\"", V, It->second));
  }
  for (auto &[K, V] : FB)
    if (!FA.count(K) && (StringRef(FlagCat(K)) != "debug" || X.O.CompareDebug))
      Add(mk(FlagCat(K), "added", Sig::Minor, "", "",
             "module flag \"" + K + "\"", "", V));

  auto Named = [&](Module &M, ModuleCanon &C) {
    std::map<std::string, std::string> R;
    for (NamedMDNode &N : M.named_metadata()) {
      StringRef Name = N.getName();
      if (Name == "llvm.module.flags")
        continue;
      std::vector<std::string> Ops;
      for (MDNode *Op : N.operands())
        Ops.push_back(C.mdText(Op));
      if (Name == "llvm.ident" || Name == "llvm.commandline")
        llvm::sort(Ops);
      R[Name.str()] = join(Ops, ", ");
    }
    return R;
  };
  auto NA = Named(MA, X.CA), NB = Named(MB, X.CB);
  auto NamedCat = [&](StringRef K) -> std::pair<const char *, Sig> {
    if (K == "llvm.ident" || K == "llvm.commandline")
      return {"config", Sig::Minor};
    if (K.starts_with("llvm.dbg"))
      return {"debug", Sig::Minor};
    return {"metadata", Sig::Minor};
  };
  for (auto &[K, V] : NA) {
    auto [Cat, S] = NamedCat(K);
    if (Cat == StringRef("debug") && !X.O.CompareDebug)
      continue;
    auto It = NB.find(K);
    if (It == NB.end())
      Add(mk(Cat, "removed", S, "", "", "!" + K, V));
    else if (It->second != V)
      Add(mk(Cat, "modified", S, "", "", "!" + K, V, It->second));
  }
  for (auto &[K, V] : NB) {
    auto [Cat, S] = NamedCat(K);
    if (Cat == StringRef("debug") && !X.O.CompareDebug)
      continue;
    if (!NA.count(K))
      Add(mk(Cat, "added", S, "", "", "!" + K, "", V));
  }

  auto Types = [](Module &M) {
    std::map<std::string, std::string> R;
    for (StructType *T : M.getIdentifiedStructTypes()) {
      std::string S;
      raw_string_ostream OS(S);
      if (T->isOpaque())
        OS << "opaque";
      else
        T->print(OS, false, false);
      R[T->getName().str()] = S;
    }
    return R;
  };
  auto TA = Types(MA), TB = Types(MB);
  for (auto &[K, V] : TA) {
    auto It = TB.find(K);
    if (It == TB.end())
      Add(mk("type", "removed", Sig::Minor, "", "", "%" + K, V));
    else if (It->second != V)
      Add(mk("type", "modified", Sig::Major, "", "", "%" + K, V, It->second));
  }
  for (auto &[K, V] : TB)
    if (!TA.count(K))
      Add(mk("type", "added", Sig::Minor, "", "", "%" + K, "", V));
}

static void globals(ModuleDiff &D, Ctx &X, FuncPair &Mapper) {
  auto Add = [&](Change C) { D.ModuleChanges.push_back(std::move(C)); };
  auto Each = [&](auto RangeA, auto RangeB) {
    for (GlobalValue &G : RangeA) {
      auto It = X.GM.AtoB.find(&G);
      std::string NA = gname(X.CA, G);
      if (It == X.GM.AtoB.end()) {
        Add(mk("global", "removed", Sig::Major, "", "", NA,
               X.CA.globalLine(G)));
        continue;
      }
      GlobalValue &H = *It->second;
      std::string NB = gname(X.CB, H);
      if (NA != NB) {
        Change C = mk("global", "renamed",
                      G.hasLocalLinkage() ? Sig::Cosmetic : Sig::Major, "", "",
                      NA + " -> " + NB);
        C.Inferred = true;
        C.Details.push_back(NA.find('.') != std::string::npos || !G.hasName()
                                ? "matched by identical content (auto-numbered "
                                  "local symbol)"
                                : "matched by identical content");
        Add(C);
      }
      auto FA = globalFields(G, X.CA), FB = globalFields(H, X.CB);
      Change C = mk("global", "modified", Sig::Major, "", "", NA,
                    X.CA.globalLine(G), X.CB.globalLine(H));
      Sig Max = Sig::Cosmetic;
      for (unsigned I = 0; I < FA.size() && I < FB.size(); ++I) {
        std::string VB = Mapper.mapGlobals(FB[I].second);
        if (FA[I].second == VB)
          continue;
        const std::string &F = FA[I].first;
        if (F == "metadata" && !X.O.CompareMetadata)
          continue;
        if (F == "attributes" && !X.O.CompareAttributes)
          continue;
        Sig S = (F == "dso-local" || F == "unnamed-addr" || F == "metadata" ||
                 F == "partition")
                    ? Sig::Minor
                    : Sig::Major;
        Max = std::max(Max, S);
        std::string VA = FA[I].second.empty() ? "(none)" : FA[I].second;
        std::string Shown = FB[I].second.empty() ? "(none)" : FB[I].second;
        if (VA.size() > 120)
          VA = VA.substr(0, 117) + "...";
        if (Shown.size() > 120)
          Shown = Shown.substr(0, 117) + "...";
        C.Details.push_back(F + ": " + VA + " -> " + Shown);
      }
      if (!C.Details.empty()) {
        C.Significance = Max;
        Add(C);
      }
    }
    for (GlobalValue &H : RangeB)
      if (!X.GM.BtoA.count(&H))
        Add(mk("global", "added", Sig::Major, "", "", gname(X.CB, H), "",
               X.CB.globalLine(H)));
  };
  Each(X.CA.M.globals(), X.CB.M.globals());
  Each(X.CA.M.aliases(), X.CB.M.aliases());
  Each(X.CA.M.ifuncs(), X.CB.M.ifuncs());
}

static std::string declLine(Function &F, ModuleCanon &C) {
  std::string S;
  for (auto &[K, V] : headerFields(F, C))
    if (K == "return-type")
      S = V + " " + globalToken(F, C);
    else if (K == "parameters")
      S += "(" + V + ")";
  return S;
}

ModuleDiff diffModules(Loaded &LA, Loaded &LB, const DiffOptions &O) {
  ModuleDiff D;
  D.A = LA.Info;
  D.B = LB.Info;
  checkCompat(D);
  for (auto &W : LA.Warnings)
    D.Compat.push_back(LA.Info.Path + ": " + W);
  for (auto &W : LB.Warnings)
    D.Compat.push_back(LB.Info.Path + ": " + W);
  ModuleCanon CA(*LA.M, O), CB(*LB.M, O);
  GlobalMatch GM = matchGlobals(CA, CB);
  std::unique_ptr<AnalysisHost> HA, HB;
  if (O.RunAnalyses) {
    HA = std::make_unique<AnalysisHost>(*LA.M);
    HB = std::make_unique<AnalysisHost>(*LB.M);
  }
  Ctx X{CA, CB, GM, O, HA.get(), HB.get()};
  for (auto &N : GM.Ambiguous) {
    Change C = mk("match", "ambiguous", Sig::Minor, "", "", N);
    C.Confidence = "ambiguous";
    D.ModuleChanges.push_back(C);
  }
  moduleLevel(D, X);
  FuncInfo Dummy;
  FuncPair Mapper(CA, Dummy, CB, Dummy, GM);
  globals(D, X, Mapper);

  auto Wanted = [&](StringRef NA, StringRef NB) {
    return O.Functions.empty() || matchesAny(O.Functions, NA.str()) ||
           matchesAny(O.Functions, NB.str());
  };
  for (Function &F : *LA.M) {
    auto It = GM.AtoB.find(&F);
    Function *G = It == GM.AtoB.end() ? nullptr : cast<Function>(It->second);
    std::string NA = gname(CA, F), NB = G ? gname(CB, *G) : "";
    if (!Wanted(NA, NB))
      continue;
    bool Intr = F.isIntrinsic();
    if (F.isDeclaration() && (!G || G->isDeclaration())) {
      if (!G) {
        D.ModuleChanges.push_back(mk(Intr ? "intrinsic" : "declaration",
                                     "removed", Intr ? Sig::Minor : Sig::Major,
                                     NA, "", NA, declLine(F, CA)));
        continue;
      }
      auto HA2 = headerFields(F, CA), HB2 = headerFields(*G, CB);
      for (unsigned I = 0; I < HA2.size(); ++I) {
        std::string VB = Mapper.mapGlobals(HB2[I].second);
        if (HA2[I].second == VB)
          continue;
        StringRef K = HA2[I].first;
        bool Attr = K.contains("attributes");
        if (Attr && !X.O.CompareAttributes && K != "config-attributes")
          continue;
        if (K == "metadata" && !X.O.CompareMetadata)
          continue;
        Change C = mk(Intr ? "intrinsic" : "declaration", "modified",
                      K == "config-attributes" || K == "metadata" ? Sig::Minor
                                                                  : Sig::Major,
                      NA, "", NA + " " + K.str(), HA2[I].second, HB2[I].second);
        if (K == "parameter-attributes")
          C.Details = paramDelta(HA2[I].second, VB);
        else if (Attr)
          C.Details = setDelta(HA2[I].second, VB);
        D.ModuleChanges.push_back(C);
      }
      continue;
    }
    FuncDiff FD;
    FD.NameA = NA;
    FD.NameB = NB;
    if (!G) {
      FD.Status = "removed";
      FD.Stats.InstsA = F.getInstructionCount();
      FD.Stats.BlocksA = F.size();
      FD.Changes.push_back(
          mk("function", "removed", Sig::Major, NA, "", NA, declLine(F, CA)));
      if (O.Rows)
        wholeFunctionRows(CA, CA.func(F), false, FD);
      D.Funcs.push_back(std::move(FD));
      continue;
    }
    if (F.isDeclaration() != G->isDeclaration()) {
      FD.Status = "modified";
      FD.Changes.push_back(
          mk("function", "modified", Sig::Major, NA, "", NA,
             F.isDeclaration() ? "declaration" : "definition",
             G->isDeclaration() ? "declaration" : "definition"));
      FD.Changes.back().Details.push_back(
          F.isDeclaration() ? "body is now defined in B"
                            : "body removed; only a declaration remains in B");
      if (O.Rows) {
        if (!F.isDeclaration())
          wholeFunctionRows(CA, CA.func(F), false, FD);
        else
          wholeFunctionRows(CB, CB.func(*G), true, FD);
      }
      D.Funcs.push_back(std::move(FD));
      continue;
    }
    FuncPair P(CA, CA.func(F), CB, CB.func(*G), GM);
    P.run();
    if (NA != NB) {
      Change C = mk("function", "renamed",
                    F.hasLocalLinkage() ? Sig::Minor : Sig::Major, NA, "",
                    NA + " -> " + NB);
      C.Inferred = true;
      C.Details.push_back("matched by structural content");
      FD.Changes.push_back(C);
    }
    FuncDiffer(X, P, FD).run();
    D.Funcs.push_back(std::move(FD));
  }
  for (Function &G : *LB.M) {
    if (GM.BtoA.count(&G))
      continue;
    std::string NB = gname(CB, G);
    if (!Wanted("", NB))
      continue;
    if (G.isDeclaration()) {
      bool Intr = G.isIntrinsic();
      D.ModuleChanges.push_back(mk(Intr ? "intrinsic" : "declaration", "added",
                                   Intr ? Sig::Minor : Sig::Major, NB, "", NB,
                                   "", declLine(G, CB)));
      continue;
    }
    FuncDiff FD;
    FD.NameB = NB;
    FD.Status = "added";
    FD.Stats.InstsB = G.getInstructionCount();
    FD.Stats.BlocksB = G.size();
    FD.Changes.push_back(
        mk("function", "added", Sig::Major, NB, "", NB, "", declLine(G, CB)));
    if (O.Rows)
      wholeFunctionRows(CB, CB.func(G), true, FD);
    D.Funcs.push_back(std::move(FD));
  }
  return D;
}

} // namespace irdiff
