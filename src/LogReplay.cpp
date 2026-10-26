#include "Pipeline.h"

#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;

namespace irdiff {

static bool diffTexts(const std::string &Before, const std::string &After,
                      const DiffOptions &O, ModuleDiff &D, std::string &Err) {
  LoadResult A = loadModuleFromBuffer(Before, "before", false);
  LoadResult B = loadModuleFromBuffer(After, "after", false);
  if (!A.L || !B.L) {
    Err = !A.L ? A.Error : B.Error;
    return false;
  }
  D = diffModules(*A.L, *B.L, O);
  D.Compat.clear();
  return true;
}

bool traceLog(const std::string &Path, const DiffOptions &O,
              std::vector<PassStep> &Steps, std::vector<std::string> &Notes,
              std::string &Err) {
  auto Buf = MemoryBuffer::getFileOrSTDIN(Path);
  if (!Buf) {
    Err = Path + ": cannot read: " + Buf.getError().message();
    return false;
  }
  struct Seg {
    std::string Kind, Pass, Unit;
    std::string Text;
    bool Omitted = false;
  };
  std::vector<Seg> Segs;
  StringRef Rest = (*Buf)->getBuffer();
  while (!Rest.empty()) {
    auto [Line, Next] = Rest.split('\n');
    Rest = Next;
    StringRef T = Line.trim();
    StringRef H = T;
    H.consume_front("; ");
    if (H.starts_with("*** IR Dump") || H.starts_with("*** IR Pass")) {
      Seg S;
      H = H.drop_front(4);
      H.consume_back(" ***");
      if (H.consume_front("IR Dump At Start")) {
        S.Kind = "Start";
      } else if (H.consume_front("IR Dump After ")) {
        S.Kind = "After";
      } else if (H.consume_front("IR Dump Before ")) {
        S.Kind = "Before";
      } else {
        S.Kind = "Other";
        S.Omitted = true;
      }
      if (H.consume_back(" omitted because no change") ||
          H.consume_back(" filtered out") || H.consume_back(" ignored"))
        S.Omitted = true;
      size_t On = H.find(" on ");
      S.Pass = H.substr(0, On).str();
      if (On != StringRef::npos)
        S.Unit = H.substr(On + 4).str();
      Segs.push_back(std::move(S));
      continue;
    }
    if (!Segs.empty())
      Segs.back().Text += Line.str() + "\n";
  }
  if (Segs.empty()) {
    Err = Path + ": no '*** IR Dump' sections found; produce the log with "
                 "-print-after-all (or -print-changed) and -print-module-scope";
    return false;
  }
  std::string Prev;
  bool HavePrev = false;
  unsigned Index = 0;
  for (Seg &S : Segs) {
    ++Index;
    if (S.Omitted || StringRef(S.Text).trim().empty())
      continue;
    StringRef Tx(S.Text);
    if (!Tx.contains("target triple") && !Tx.contains("source_filename") &&
        !Tx.contains("; ModuleID")) {
      Err = Path + ": dump for '" + S.Pass +
            "' is not module-scoped; rerun "
            "with -print-module-scope";
      return false;
    }
    if (S.Kind == "Start" || S.Kind == "Before") {
      Prev = S.Text;
      HavePrev = true;
      continue;
    }
    if (!HavePrev) {
      Notes.push_back("no initial snapshot before '" + S.Pass +
                      "'; its changes cannot be attributed");
      Prev = S.Text;
      HavePrev = true;
      continue;
    }
    PassStep St;
    St.Run.Index = Index;
    St.Run.Pass = S.Pass;
    St.Run.Unit = S.Unit;
    std::string E;
    if (!diffTexts(Prev, S.Text, O, St.Diff, E)) {
      Err = Path + ": cannot parse dump after '" + S.Pass + "': " + E;
      return false;
    }
    bool Any = !St.Diff.ModuleChanges.empty();
    for (auto &F : St.Diff.Funcs)
      Any |= !F.Changes.empty();
    St.Run.Changed = Any;
    Prev = std::move(S.Text);
    if (Any)
      Steps.push_back(std::move(St));
  }
  return true;
}

} // namespace irdiff
