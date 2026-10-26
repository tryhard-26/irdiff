#include "DiffDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace irdiff {
namespace detail {

Change mk(StringRef Cat, StringRef Kind, Sig S, StringRef Func, StringRef Block,
          std::string Subject, std::string Before, std::string After) {
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

// flags as printed, read from the inst itself (not constexpr operands)
std::multiset<std::string> instFlags(const Instruction &I) {
  std::multiset<std::string> R;
  auto Add = [&](bool On, StringRef W) {
    if (On && !R.count(W.str()))
      R.insert(W.str());
  };
  auto Scope = [&](SyncScope::ID ID) {
    if (ID == SyncScope::System)
      return;
    SmallVector<StringRef, 8> Names;
    I.getContext().getSyncScopeNames(Names);
    R.insert("syncscope(\"" +
             (ID < Names.size() ? Names[ID] : StringRef("?")).str() + "\")");
  };
  if (auto *O = dyn_cast<OverflowingBinaryOperator>(&I)) {
    Add(O->hasNoUnsignedWrap(), "nuw");
    Add(O->hasNoSignedWrap(), "nsw");
  }
  if (auto *T = dyn_cast<TruncInst>(&I)) {
    Add(T->hasNoUnsignedWrap(), "nuw");
    Add(T->hasNoSignedWrap(), "nsw");
  }
  if (auto *E = dyn_cast<PossiblyExactOperator>(&I))
    Add(E->isExact(), "exact");
  if (auto *D = dyn_cast<PossiblyDisjointInst>(&I))
    Add(D->isDisjoint(), "disjoint");
  if (auto *N = dyn_cast<PossiblyNonNegInst>(&I))
    Add(N->hasNonNeg(), "nneg");
#if LLVM_VERSION_MAJOR >= 20
  if (auto *C = dyn_cast<ICmpInst>(&I))
    Add(C->hasSameSign(), "samesign");
#endif
  if (auto *G = dyn_cast<GetElementPtrInst>(&I)) {
    GEPNoWrapFlags NW = G->getNoWrapFlags();
    Add(NW.isInBounds(), "inbounds");
    Add(NW.hasNoUnsignedSignedWrap() && !NW.isInBounds(), "nusw");
    Add(NW.hasNoUnsignedWrap(), "nuw");
  }
  if (isa<FPMathOperator>(&I)) {
    FastMathFlags F = I.getFastMathFlags();
    if (F.isFast()) {
      Add(true, "fast");
    } else {
      Add(F.allowReassoc(), "reassoc");
      Add(F.noNaNs(), "nnan");
      Add(F.noInfs(), "ninf");
      Add(F.noSignedZeros(), "nsz");
      Add(F.allowReciprocal(), "arcp");
      Add(F.allowContract(), "contract");
      Add(F.approxFunc(), "afn");
    }
  }
  if (auto *C = dyn_cast<CallInst>(&I)) {
    Add(C->getTailCallKind() == CallInst::TCK_Tail, "tail");
    Add(C->getTailCallKind() == CallInst::TCK_MustTail, "musttail");
    Add(C->getTailCallKind() == CallInst::TCK_NoTail, "notail");
  }
  if (auto *L = dyn_cast<LoadInst>(&I)) {
    Add(L->isVolatile(), "volatile");
    if (L->isAtomic()) {
      Add(true, "atomic");
      R.insert(toIRString(L->getOrdering()));
      Scope(L->getSyncScopeID());
    }
  } else if (auto *S = dyn_cast<StoreInst>(&I)) {
    Add(S->isVolatile(), "volatile");
    if (S->isAtomic()) {
      Add(true, "atomic");
      R.insert(toIRString(S->getOrdering()));
      Scope(S->getSyncScopeID());
    }
  } else if (auto *A = dyn_cast<AtomicRMWInst>(&I)) {
    Add(A->isVolatile(), "volatile");
    R.insert(toIRString(A->getOrdering()));
    Scope(A->getSyncScopeID());
  } else if (auto *X = dyn_cast<AtomicCmpXchgInst>(&I)) {
    Add(X->isVolatile(), "volatile");
    Add(X->isWeak(), "weak");
    R.insert(toIRString(X->getSuccessOrdering()));
    R.insert(toIRString(X->getFailureOrdering()));
    Scope(X->getSyncScopeID());
  } else if (auto *F = dyn_cast<FenceInst>(&I)) {
    R.insert(toIRString(F->getOrdering()));
    Scope(F->getSyncScopeID());
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

} // namespace detail

std::string blockName(const FuncInfo &F, unsigned Blk) {
  StringRef L = F.Blocks[Blk].Label;
  return L.starts_with("%") ? L.drop_front().str() : L.str();
}

std::string instText(ModuleCanon &C, const FuncInfo &F, unsigned I) {
  return C.display(F, detail::stripFnAttrs(F.Insts[I].Canon));
}

} // namespace irdiff
