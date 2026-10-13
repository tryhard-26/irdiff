#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace irdiff {

enum class Sig { Cosmetic = 0, Minor = 1, Major = 2 };
const char *sigName(Sig S);

// cats for --category
extern const std::vector<std::string> AllCategories;
// kinds for --change
extern const std::vector<std::string> AllKinds;
// analyses for --analysis
extern const std::vector<std::string> AllAnalyses;

struct Change {
  std::string Cat;
  std::string Kind;
  Sig Significance = Sig::Major;
  std::string Func;
  std::string Block;
  std::string Subject;
  std::string Before, After;
  std::vector<std::string> Details;
  bool Inferred = false;
  std::string Confidence;
  std::string Pass;
};

// one line of aligned fn listing
struct Row {
  enum Tag { Same, Del, Add, Mod, Label } T = Same;
  std::string Left, Right, Note;
};

struct FuncStats {
  unsigned BlocksA = 0, BlocksB = 0, InstsA = 0, InstsB = 0;
  unsigned Added = 0, Removed = 0, Modified = 0, Moved = 0, Unchanged = 0;
};

struct FuncDiff {
  std::string NameA, NameB;
  std::string Status;
  bool Ambiguous = false;
  FuncStats Stats;
  std::vector<Row> Rows;
  std::vector<Change> Changes;
};

struct InputInfo {
  std::string Path;
  std::string Format;
  std::string Producer;
  int ProducerMajor = 0;
  std::string Triple;
  std::string DataLayout;
  std::string SourceFile;
  unsigned Functions = 0, Declarations = 0, Globals = 0, Instructions = 0;
};

struct Remark {
  std::string Type, Pass, Name, Function, Loc, Message;
  std::string Origin;
};

struct PassRun {
  unsigned Index = 0;
  std::string Pass;
  std::string Short;
  std::string Unit;
  bool Changed = false;
};

struct ModuleDiff {
  InputInfo A, B;
  std::vector<std::string> Compat;
  bool Incompatible = false;
  std::vector<Change> ModuleChanges;
  std::vector<FuncDiff> Funcs;
  std::vector<Change> RemarkChanges;
  std::vector<Change> PassChanges;
  std::string Label;
};

enum class Names { Original, Canonical };

struct Options {
  std::vector<std::string> Functions;
  std::vector<std::string> Blocks;
  std::vector<std::string> Categories;
  std::vector<std::string> Kinds;
  std::vector<std::string> Passes;
  std::vector<std::string> Analyses;
  Sig MinSig = Sig::Minor;
  bool CompareDebug = false;
  bool CompareMetadata = true;
  bool CompareAttributes = true;
  bool IgnoreLayout = true;
  bool Strict = false;
  bool Verify = true;
  bool RunAnalyses = false;
  unsigned AALimit = 48;
  Names NameMode = Names::Original;
};

// cat/kind/sig/blk/pass filters
bool keepChange(const Change &C, const Options &O);
bool matchesAny(const std::vector<std::string> &Globs, const std::string &S);

} // namespace irdiff
