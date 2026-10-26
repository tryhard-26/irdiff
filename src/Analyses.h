#pragma once

#include "Input.h"
#include "Match.h"
#include "llvm/IR/PassManager.h"
#include <functional>

namespace irdiff {

class AnalysisHost {
public:
  explicit AnalysisHost(llvm::Module &M);
  ~AnalysisHost();
  llvm::FunctionAnalysisManager &fam();

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

void diffAnalyses(FuncPair &P, FuncDiff &FD, const Options &O, AnalysisHost &HA,
                  AnalysisHost &HB);

// per-fn facts as "observation" changes
std::vector<FuncDiff> analyzeModule(Loaded &L, const Options &O);

void installRemarkCollector(llvm::LLVMContext &Ctx, std::vector<Remark> &Out,
                            std::function<std::string()> CurrentPass);
bool loadRemarks(const std::string &Path, std::vector<Remark> &Out,
                 std::string &Err);
void diffRemarks(const std::vector<Remark> &A, const std::vector<Remark> &B,
                 ModuleDiff &D, const Options &O);
// evidence-backed hints for missed-opt investigation
void addHints(ModuleDiff &D);

} // namespace irdiff
