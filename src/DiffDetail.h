#pragma once

#include "Analyses.h"
#include "Differ.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include <map>
#include <set>

// internal to the differ; split across DiffUtil/FuncDiffer*/Differ.cpp
namespace irdiff::detail {

Change mk(llvm::StringRef Cat, llvm::StringRef Kind, Sig S,
          llvm::StringRef Func, llvm::StringRef Block, std::string Subject,
          std::string Before = "", std::string After = "");
std::string typeStr(llvm::Type *T);
std::vector<std::string> attrTokens(llvm::StringRef S);
std::vector<std::string> setDelta(llvm::StringRef A, llvm::StringRef B);
std::vector<std::string> paramDelta(llvm::StringRef A, llvm::StringRef B);
std::multiset<std::string> instFlags(const llvm::Instruction &I);
std::string alignOf(llvm::StringRef S);
std::string fnAttrsOf(llvm::StringRef S);
std::string stripFnAttrs(llvm::StringRef S);

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
  std::unique_ptr<llvm::DominatorTree> DTA, DTB;
  std::unique_ptr<llvm::LoopInfo> LIA, LIB;

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
  bool equivalent(unsigned IA, unsigned IB, std::string &Why,
                  std::string &Kind);
  bool sameAddress(bool SideB, const llvm::Value *V, std::string &Key);
  void classify(unsigned IA, unsigned IB, bool Moved);
  void blocks();
  void cfg();
  void loops();
  void debug();
  void rows();
  std::string canonText(bool SideB, unsigned I);
};

} // namespace irdiff::detail
