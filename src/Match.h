#pragma once

#include "Canon.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/GlobalValue.h"

namespace irdiff {

using Pairs = std::vector<std::pair<unsigned, unsigned>>;

// Myers LCS of hash seqs; middle dropped beyond MaxD edits
Pairs lcs(llvm::ArrayRef<uint64_t> A, llvm::ArrayRef<uint64_t> B,
          unsigned MaxD = 2000);
double similarity(llvm::ArrayRef<uint64_t> A, llvm::ArrayRef<uint64_t> B);

enum How : uint8_t {
  HowNone,
  HowAligned,
  HowUnique,
  HowDefUse,
  HowDefUseLoose,
  HowPosition
};

struct GlobalMatch {
  llvm::DenseMap<llvm::GlobalValue *, llvm::GlobalValue *> AtoB, BtoA;
  // B tok -> A tok for renamed/unnamed globals
  llvm::StringMap<std::string> TokBtoA;
  std::vector<std::pair<llvm::GlobalValue *, llvm::GlobalValue *>> Renamed;
  std::vector<std::string> Ambiguous;
};

GlobalMatch matchGlobals(ModuleCanon &A, ModuleCanon &B);
std::string globalToken(const llvm::GlobalValue &G, ModuleCanon &C);

class FuncPair {
public:
  FuncPair(ModuleCanon &CA, FuncInfo &A, ModuleCanon &CB, FuncInfo &B,
           const GlobalMatch &G);
  ModuleCanon &CA, &CB;
  FuncInfo &A, &B;
  const GlobalMatch &G;
  std::vector<int> VA, VB;

  void run();
  std::string strictA(unsigned I) const;
  std::string strictB(unsigned I) const;
  std::string mapGlobals(llvm::StringRef S) const;
  std::string strictText(bool SideB, llvm::StringRef Canon) const;
  std::vector<uint64_t> blockShapes(bool SideB, unsigned Blk) const;

private:
  std::vector<std::string> ShapeA, ShapeB;
  void link(unsigned IA, unsigned IB, How H);
  void unlink(unsigned IA);
  void matchBlocks();
  void alignBlocks(unsigned Round);
  void crossMatch();
  void contentAlign();
  void positional();
  bool defUse(bool Loose);
  std::string key(bool SideB, unsigned I, bool Wild) const;
};

} // namespace irdiff
