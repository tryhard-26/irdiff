#pragma once

#include "Pipeline.h"
#include "llvm/ADT/Any.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Target/TargetMachine.h"
#include <functional>

// internal to the pipeline code; split across PassRunner/Trace/LogReplay.cpp
namespace irdiff::detail {

struct Frame {
  std::string Pass, Unit;
  bool ModuleUnit = false;
  std::vector<std::pair<const llvm::Function *, uint64_t>> Funcs;
  uint64_t ModHash = 0;
  size_t Counts = 0, GlobalCounts = 0;
  bool Infra = false;
};

// tracks pass executions via instrumentation and detects IR changes
struct Runner {
  llvm::Module &M;
  const PipelineConfig &C;
  PipelineResult &R;
  std::function<void(const PassRun &, const std::vector<std::string> &, bool,
                     bool)>
      OnChange;
  std::vector<Frame> Stack;
  unsigned Index = 0;
  llvm::PassInstrumentationCallbacks *PIC = nullptr;

  void before(llvm::StringRef P, const llvm::Any &IR);
  void after(llvm::StringRef P, const llvm::Any *IR, bool Invalidated);
  std::string current() const;
};

std::unique_ptr<llvm::TargetMachine> makeTM(llvm::Module &M,
                                            const PipelineConfig &C,
                                            std::vector<std::string> &Notes);
bool run(llvm::Module &M, const PipelineConfig &C, PipelineResult &R,
         std::string &Err, Runner *Rn, llvm::TargetMachine *TM);

} // namespace irdiff::detail
