#pragma once

#include "Input.h"
#include "Match.h"

namespace irdiff {

struct DiffOptions : Options {
  bool Rows = false;
  // module-level state and declarations only; skip definitions
  bool ModuleOnly = false;
};

ModuleDiff diffModules(Loaded &A, Loaded &B, const DiffOptions &O);

// blk/inst labels for user-facing text
std::string blockName(const FuncInfo &F, unsigned B);
std::string instText(ModuleCanon &C, const FuncInfo &F, unsigned I);

} // namespace irdiff
