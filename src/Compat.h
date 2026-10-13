#pragma once

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

namespace irdiff {

inline std::string tripleOf(const llvm::Module &M) {
#if LLVM_VERSION_MAJOR >= 21
  return M.getTargetTriple().str();
#else
  return M.getTargetTriple();
#endif
}

inline const llvm::Target *lookupTarget(const llvm::Triple &T,
                                        std::string &Err) {
#if LLVM_VERSION_MAJOR >= 21
  return llvm::TargetRegistry::lookupTarget(T, Err);
#else
  return llvm::TargetRegistry::lookupTarget(T.str(), Err);
#endif
}

inline llvm::TargetMachine *createTM(const llvm::Target *Tgt,
                                     const llvm::Triple &T, llvm::StringRef CPU,
                                     llvm::StringRef Features,
                                     llvm::CodeGenOptLevel OL) {
  llvm::TargetOptions TO;
#if LLVM_VERSION_MAJOR >= 21
  return Tgt->createTargetMachine(T, CPU, Features, TO, std::nullopt,
                                  std::nullopt, OL);
#else
  return Tgt->createTargetMachine(T.str(), CPU, Features, TO, std::nullopt,
                                  std::nullopt, OL);
#endif
}

} // namespace irdiff
