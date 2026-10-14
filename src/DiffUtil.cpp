#include "DiffDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
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

} // namespace detail

std::string blockName(const FuncInfo &F, unsigned Blk) {
  StringRef L = F.Blocks[Blk].Label;
  return L.starts_with("%") ? L.drop_front().str() : L.str();
}

std::string instText(ModuleCanon &C, const FuncInfo &F, unsigned I) {
  return C.display(F, detail::stripFnAttrs(F.Insts[I].Canon));
}

} // namespace irdiff
