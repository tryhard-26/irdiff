#pragma once

#include "Pipeline.h"
#include "llvm/Support/raw_ostream.h"

namespace irdiff {

enum class Format { Text, Unified, Side, Json };

struct ReportOptions {
  Format F = Format::Text;
  bool Color = false;
  bool Verbose = false;
  bool Summary = false;
  unsigned Context = 3;
  unsigned Width = 160;
  std::string Command;
};

struct Totals {
  unsigned Kept = 0, Hidden = 0;
  unsigned BySig[3] = {0, 0, 0};
  std::map<std::string, unsigned> ByCat, ByKind;
  unsigned Compared = 0, Identical = 0, Equivalent = 0, Modified = 0, Added = 0,
           Removed = 0;
  Sig Max = Sig::Cosmetic;
};

Totals totals(const ModuleDiff &D, const Options &O);
bool funcHasKept(const FuncDiff &F, const Options &O);

void printDiff(llvm::raw_ostream &OS, const ModuleDiff &D, const Options &O,
               const ReportOptions &R);
void printPasses(llvm::raw_ostream &OS, const InputInfo &In,
                 const std::string &Pipeline,
                 const std::vector<PassStep> &Steps, const PipelineResult *PR,
                 const std::vector<std::string> &Notes, const Options &O,
                 const ReportOptions &R);
void printAnalyze(llvm::raw_ostream &OS, const InputInfo &In,
                  const std::vector<FuncDiff> &Fs, const Options &O,
                  const ReportOptions &R);

bool stepKept(const PassStep &S, const Options &O);

} // namespace irdiff
