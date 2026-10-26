#pragma once

#include "Model.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <memory>

namespace irdiff {

enum Exit {
  ExitSame = 0,
  ExitDiffer = 1,
  ExitUsage = 2,
  ExitInput = 3,
  ExitIncompatible = 4,
  ExitInternal = 5
};

struct Loaded {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> M;
  InputInfo Info;
  std::vector<std::string> Warnings;
};

struct LoadResult {
  std::unique_ptr<Loaded> L;
  int Code = ExitSame;
  std::string Error;
};

LoadResult loadModule(const std::string &Path, bool Verify);
LoadResult loadModuleFromBuffer(llvm::StringRef Text, llvm::StringRef Name,
                                bool Verify);
void fillInfo(Loaded &L);

// pairwise compat notes; sets Incompatible on target mismatch
void checkCompat(ModuleDiff &D);

int llvmMajor();
std::string llvmVersion();
int parseProducerMajor(llvm::StringRef Producer);

} // namespace irdiff
