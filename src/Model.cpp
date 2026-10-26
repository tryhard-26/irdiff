#include "Model.h"

#include "llvm/Support/GlobPattern.h"

namespace irdiff {

const char *sigName(Sig S) {
  switch (S) {
  case Sig::Cosmetic:
    return "cosmetic";
  case Sig::Minor:
    return "minor";
  case Sig::Major:
    return "major";
  }
  return "?";
}

const std::vector<std::string> AllCategories = {
    "module",   "type",        "global",  "declaration", "intrinsic",
    "function", "signature",   "linkage", "attribute",   "config",
    "block",    "instruction", "cfg",     "loop",        "metadata",
    "debug",    "analysis",    "remark",  "pass",        "match",
    "hint"};

const std::vector<std::string> AllKinds = {
    "added",          "removed",          "modified",
    "opcode-changed", "operands-changed", "flags-changed",
    "type-changed",   "metadata-changed", "moved",
    "reordered",      "commuted",         "merged",
    "split",          "renamed",          "rewritten",
    "unreachable",    "ambiguous",        "observation"};

const std::vector<std::string> AllAnalyses = {"dom", "loops", "scev", "aa",
                                              "mssa"};

bool matchesAny(const std::vector<std::string> &Globs, const std::string &S) {
  for (const std::string &G : Globs) {
    auto P = llvm::GlobPattern::create(G);
    if (!P) {
      llvm::consumeError(P.takeError());
      if (G == S)
        return true;
      continue;
    }
    if (P->match(S))
      return true;
    if (!S.empty() && (S[0] == '@' || S[0] == '%') && P->match(S.substr(1)))
      return true;
  }
  return false;
}

static bool listed(const std::vector<std::string> &L, const std::string &S) {
  for (const std::string &X : L)
    if (X == S)
      return true;
  return false;
}

bool keepChange(const Change &C, const Options &O) {
  if (C.Significance < O.MinSig)
    return false;
  if (!O.Categories.empty() && !listed(O.Categories, C.Cat))
    return false;
  if (!O.Kinds.empty() && !listed(O.Kinds, C.Kind))
    return false;
  if (!O.Blocks.empty() && !matchesAny(O.Blocks, C.Block))
    return false;
  if (!O.Passes.empty() && !C.Pass.empty() && !matchesAny(O.Passes, C.Pass))
    return false;
  return true;
}

} // namespace irdiff
