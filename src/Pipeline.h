#pragma once

#include "Differ.h"

namespace irdiff {

struct PipelineConfig {
  std::string Passes = "default<O2>";
  std::string CPU, Features;
  bool UseTarget = true;
  bool VerifyEach = false;
};

struct PipelineResult {
  std::vector<PassRun> Runs;
  std::vector<Remark> Remarks;
  std::string PipelineText;
  std::vector<std::string> Notes;
};

struct PassStep {
  PassRun Run;
  std::vector<std::string> Functions;
  ModuleDiff Diff;
  std::vector<Remark> Remarks;
};

// run textual pipeline on M in place
bool runPipeline(llvm::Module &M, const PipelineConfig &C, PipelineResult &R,
                 std::string &Err);

// run pipeline, diff around each changing pass
bool tracePasses(Loaded &L, const PipelineConfig &C, const DiffOptions &O,
                 const std::string &SaveDir, std::vector<PassStep> &Steps,
                 PipelineResult &R, std::string &Err);

// replay -print-after-all/-print-changed log (module scope)
bool traceLog(const std::string &Path, const DiffOptions &O,
              std::vector<PassStep> &Steps, std::vector<std::string> &Notes,
              std::string &Err);

// pipeline structure + exec-count diffs
void diffPipelines(const PipelineResult &A, const PipelineResult &B,
                   ModuleDiff &D);

std::vector<std::string> flattenPipeline(llvm::StringRef Text);

} // namespace irdiff
