#include "Report.h"
#include "Version.h"

#include "llvm/Support/JSON.h"
#include <set>

using namespace llvm;

namespace irdiff {

static bool kept(const Change &C, const Options &O) { return keepChange(C, O); }

bool funcHasKept(const FuncDiff &F, const Options &O) {
  for (auto &C : F.Changes)
    if (kept(C, O))
      return true;
  return false;
}

Totals totals(const ModuleDiff &D, const Options &O) {
  Totals T;
  auto Count = [&](const Change &C) {
    if (!kept(C, O)) {
      ++T.Hidden;
      return;
    }
    ++T.Kept;
    ++T.BySig[int(C.Significance)];
    ++T.ByCat[C.Cat];
    ++T.ByKind[C.Kind];
    T.Max = std::max(T.Max, C.Significance);
  };
  for (auto &C : D.ModuleChanges)
    Count(C);
  for (auto &C : D.RemarkChanges)
    Count(C);
  for (auto &C : D.PassChanges)
    Count(C);
  for (auto &F : D.Funcs) {
    for (auto &C : F.Changes)
      Count(C);
    if (F.Status == "added")
      ++T.Added;
    else if (F.Status == "removed")
      ++T.Removed;
    else {
      ++T.Compared;
      if (F.Status == "identical")
        ++T.Identical;
      else if (F.Status == "equivalent")
        ++T.Equivalent;
      else
        ++T.Modified;
    }
  }
  return T;
}

namespace {

struct Painter {
  raw_ostream &OS;
  bool On;
  void col(raw_ostream::Colors C, bool Bold = false) {
    if (On)
      OS.changeColor(C, Bold);
  }
  void bold() {
    if (On)
      OS.changeColor(raw_ostream::SAVEDCOLOR, true);
  }
  void reset() {
    if (On)
      OS.resetColor();
  }
};

char sym(const Change &C) {
  StringRef K = C.Kind;
  if (K == "added")
    return '+';
  if (K == "removed")
    return '-';
  if (K == "moved" || K == "reordered")
    return '>';
  if (K == "merged" || K == "split")
    return '*';
  if (K == "renamed" || K == "commuted")
    return '=';
  if (K == "unreachable")
    return '!';
  if (K == "ambiguous")
    return '?';
  if (K == "observation")
    return '.';
  return '~';
}

raw_ostream::Colors colorOf(char S) {
  switch (S) {
  case '+':
    return raw_ostream::GREEN;
  case '-':
    return raw_ostream::RED;
  case '~':
    return raw_ostream::YELLOW;
  case '!':
  case '?':
    return raw_ostream::MAGENTA;
  default:
    return raw_ostream::CYAN;
  }
}

void printChange(Painter &P, const Change &C, const ReportOptions &R,
                 StringRef Indent) {
  raw_ostream &OS = P.OS;
  char S = sym(C);
  OS << Indent;
  P.col(colorOf(S), true);
  OS << S;
  P.reset();
  OS << " [" << C.Cat << "] ";
  if (!C.Block.empty() &&
      (C.Cat == "instruction" || C.Cat == "metadata" || C.Cat == "debug" ||
       (C.Cat == "analysis" && C.Subject.rfind("immediate", 0) != 0)))
    OS << "in %" << C.Block << ": ";
  if (C.Significance == Sig::Major)
    P.bold();
  OS << C.Subject;
  P.reset();
  bool TwoLine = !C.After.empty() && C.Before == C.Subject;
  if (!TwoLine && (!C.Before.empty() || !C.After.empty())) {
    std::string B = C.Before.empty() ? "(none)" : C.Before;
    std::string A = C.After.empty() ? "(none)" : C.After;
    if (C.Kind == "added")
      OS << ": " << A;
    else if (C.Kind == "removed")
      OS << ": " << B;
    else if (B.size() + A.size() < 100 && B.find('\n') == std::string::npos)
      OS << ": " << B << " -> " << A;
    else
      OS << "\n"
         << Indent << "    A: " << B << "\n"
         << Indent << "    B: " << A;
  }
  if (C.Inferred) {
    P.col(raw_ostream::BLUE);
    OS << " [inferred]";
    P.reset();
  }
  if (!C.Confidence.empty()) {
    P.col(raw_ostream::MAGENTA);
    OS << " [" << C.Confidence << "]";
    P.reset();
  }
  if (!C.Pass.empty())
    OS << " [" << C.Pass << "]";
  if (R.Verbose)
    OS << " (" << sigName(C.Significance) << ")";
  OS << "\n";
  if (TwoLine)
    OS << Indent << "    => " << C.After << "\n";
  unsigned Max = R.Verbose ? ~0u : 3;
  for (unsigned I = 0; I < C.Details.size(); ++I) {
    if (I == Max) {
      OS << Indent << "    (" << (C.Details.size() - Max)
         << " more; use --verbose)\n";
      break;
    }
    OS << Indent << "    " << C.Details[I] << "\n";
  }
}

std::string inputLine(const InputInfo &I) {
  std::string S = I.Path + "  [" + I.Format;
  if (!I.Triple.empty())
    S += ", " + I.Triple;
  if (!I.Producer.empty())
    S += ", " + I.Producer;
  S += ", " + std::to_string(I.Functions) + " functions, " +
       std::to_string(I.Instructions) + " instructions]";
  return S;
}

void printSummary(Painter &P, const ModuleDiff &D, const Totals &T) {
  raw_ostream &OS = P.OS;
  P.bold();
  OS << "summary:";
  P.reset();
  OS << " " << T.Compared << " function(s) compared: " << T.Modified
     << " modified, " << T.Equivalent << " equivalent, " << T.Identical
     << " identical; " << T.Added << " added, " << T.Removed << " removed\n";
  OS << "  " << T.Kept << " change(s) shown (" << T.BySig[2] << " major, "
     << T.BySig[1] << " minor, " << T.BySig[0] << " cosmetic)";
  if (T.Hidden)
    OS << ", " << T.Hidden << " hidden by filters";
  OS << "\n";
  if (!T.ByCat.empty()) {
    OS << "  by category:";
    for (auto &[K, N] : T.ByCat)
      OS << " " << K << "=" << N;
    OS << "\n  by kind:";
    for (auto &[K, N] : T.ByKind)
      OS << " " << K << "=" << N;
    OS << "\n";
  }
}

void printText(Painter &P, const ModuleDiff &D, const Options &O,
               const ReportOptions &R) {
  raw_ostream &OS = P.OS;
  Totals T = totals(D, O);
  if (!D.Label.empty())
    OS << D.Label << "\n";
  P.col(raw_ostream::RED, true);
  OS << "--- A: ";
  P.reset();
  OS << inputLine(D.A) << "\n";
  P.col(raw_ostream::GREEN, true);
  OS << "+++ B: ";
  P.reset();
  OS << inputLine(D.B) << "\n";
  for (auto &N : D.Compat) {
    P.col(raw_ostream::MAGENTA);
    OS << "note: ";
    P.reset();
    OS << N << "\n";
  }
  if (!R.Summary) {
    std::vector<const Change *> Hints, Mod;
    for (auto &C : D.ModuleChanges)
      if (kept(C, O))
        (C.Cat == "hint" ? Hints : Mod).push_back(&C);
    if (!Mod.empty()) {
      OS << "\n";
      P.bold();
      OS << "module\n";
      P.reset();
      for (auto *C : Mod)
        printChange(P, *C, R, "  ");
    }
    for (auto &F : D.Funcs) {
      if (!funcHasKept(F, O))
        continue;
      OS << "\n";
      P.bold();
      OS << (F.NameA.empty() ? F.NameB : F.NameA);
      P.reset();
      if (!F.NameA.empty() && !F.NameB.empty() && F.NameA != F.NameB)
        OS << " -> " << F.NameB;
      OS << "  " << F.Status;
      const FuncStats &S = F.Stats;
      if (F.Status != "added" && F.Status != "removed")
        OS << "  (blocks " << S.BlocksA << " -> " << S.BlocksB
           << ", instructions " << S.InstsA << " -> " << S.InstsB << ": +"
           << S.Added << " -" << S.Removed << " ~" << S.Modified << " >"
           << S.Moved << ")";
      if (F.Ambiguous) {
        P.col(raw_ostream::MAGENTA);
        OS << " [ambiguous matching]";
        P.reset();
      }
      OS << "\n";
      for (auto &C : F.Changes)
        if (kept(C, O))
          printChange(P, C, R, "  ");
    }
    std::vector<const Change *> Other;
    for (auto &C : D.RemarkChanges)
      if (kept(C, O))
        Other.push_back(&C);
    if (!Other.empty()) {
      OS << "\n";
      P.bold();
      OS << "optimization remarks (as reported by LLVM)\n";
      P.reset();
      for (auto *C : Other) {
        printChange(P, *C, R, "  ");
      }
    }
    Other.clear();
    for (auto &C : D.PassChanges)
      if (kept(C, O))
        Other.push_back(&C);
    if (!Other.empty()) {
      OS << "\n";
      P.bold();
      OS << "pass pipeline\n";
      P.reset();
      for (auto *C : Other)
        printChange(P, *C, R, "  ");
    }
    if (!Hints.empty()) {
      OS << "\n";
      P.bold();
      OS << "potentially relevant differences\n";
      P.reset();
      OS << "  irdiff correlates evidence; it does not establish why an "
            "optimization did or did not happen.\n";
      for (auto *C : Hints)
        printChange(P, *C, R, "  ");
    }
    OS << "\n";
  }
  printSummary(P, D, T);
}

std::vector<std::pair<unsigned, unsigned>> hunks(const std::vector<Row> &Rows,
                                                 unsigned Ctx) {
  std::vector<std::pair<unsigned, unsigned>> H;
  for (unsigned I = 0; I < Rows.size(); ++I) {
    if (Rows[I].T == Row::Same || Rows[I].T == Row::Label)
      continue;
    unsigned B = I >= Ctx ? I - Ctx : 0;
    unsigned E = std::min<unsigned>(Rows.size(), I + Ctx + 1);
    if (!H.empty() && B <= H.back().second)
      H.back().second = std::max(H.back().second, E);
    else
      H.push_back({B, E});
  }
  return H;
}

void moduleRows(const ModuleDiff &D, const Options &O, std::vector<Row> &Out) {
  for (auto &C : D.ModuleChanges) {
    if (!kept(C, O) || C.Cat == "hint")
      continue;
    Row R;
    if (C.Kind == "added") {
      R.T = Row::Add;
      R.Right = C.After.empty() ? C.Subject : C.After;
      Out.push_back(R);
    } else if (C.Kind == "removed") {
      R.T = Row::Del;
      R.Left = C.Before.empty() ? C.Subject : C.Before;
      Out.push_back(R);
    } else {
      R.T = Row::Mod;
      R.Left = C.Before.empty() ? C.Subject : C.Before;
      R.Right = C.After.empty() ? C.Subject : C.After;
      if (C.Before.empty() && C.After.empty())
        R.Note = C.Kind;
      Out.push_back(R);
    }
  }
}

void printUnified(Painter &P, const ModuleDiff &D, const Options &O,
                  const ReportOptions &R) {
  raw_ostream &OS = P.OS;
  P.col(raw_ostream::RED, true);
  OS << "--- " << D.A.Path << "\n";
  P.col(raw_ostream::GREEN, true);
  OS << "+++ " << D.B.Path << "\n";
  P.reset();
  auto Emit = [&](StringRef Title, const std::vector<Row> &Rows) {
    for (auto [B, E] : hunks(Rows, R.Context)) {
      P.col(raw_ostream::CYAN);
      OS << "@@ " << Title << " @@\n";
      P.reset();
      for (unsigned I = B; I < E; ++I) {
        const Row &Rw = Rows[I];
        std::string Note = Rw.Note.empty() ? "" : "  ; " + Rw.Note;
        switch (Rw.T) {
        case Row::Same:
        case Row::Label:
          OS << " " << Rw.Left << Note << "\n";
          break;
        case Row::Del:
          P.col(raw_ostream::RED);
          OS << "-" << Rw.Left << Note << "\n";
          P.reset();
          break;
        case Row::Add:
          P.col(raw_ostream::GREEN);
          OS << "+" << Rw.Right << Note << "\n";
          P.reset();
          break;
        case Row::Mod:
          P.col(raw_ostream::RED);
          OS << "-" << Rw.Left << "\n";
          P.col(raw_ostream::GREEN);
          OS << "+" << Rw.Right << Note << "\n";
          P.reset();
          break;
        }
      }
    }
  };
  std::vector<Row> Mod;
  moduleRows(D, O, Mod);
  Emit("module", Mod);
  for (auto &F : D.Funcs)
    if (funcHasKept(F, O))
      Emit("define " + (F.NameA.empty() ? F.NameB : F.NameA), F.Rows);
}

std::string fit(StringRef S, unsigned W) {
  if (S.size() <= W)
    return S.str() + std::string(W - S.size(), ' ');
  if (W < 2)
    return S.substr(0, W).str();
  return S.substr(0, W - 1).str() + "~";
}

void printSide(Painter &P, const ModuleDiff &D, const Options &O,
               const ReportOptions &R) {
  raw_ostream &OS = P.OS;
  unsigned Col = R.Width > 20 ? (R.Width - 3) / 2 : 40;
  OS << fit("A: " + D.A.Path, Col) << "   " << "B: " << D.B.Path << "\n";
  auto Emit = [&](StringRef Title, const std::vector<Row> &Rows) {
    std::vector<std::pair<unsigned, unsigned>> H;
    if (R.Verbose)
      H.push_back({0, unsigned(Rows.size())});
    else
      H = hunks(Rows, R.Context);
    if (H.empty())
      return;
    P.bold();
    OS << "=== " << Title << " ===\n";
    P.reset();
    for (auto [B, E] : H) {
      for (unsigned I = B; I < E; ++I) {
        const Row &Rw = Rows[I];
        char M = ' ';
        raw_ostream::Colors C = raw_ostream::SAVEDCOLOR;
        if (Rw.T == Row::Del) {
          M = '<';
          C = raw_ostream::RED;
        } else if (Rw.T == Row::Add) {
          M = '>';
          C = raw_ostream::GREEN;
        } else if (Rw.T == Row::Mod) {
          M = '|';
          C = raw_ostream::YELLOW;
        }
        if (C != raw_ostream::SAVEDCOLOR)
          P.col(C);
        OS << fit(Rw.Left, Col) << ' ' << M << ' '
           << (Rw.Note.empty() ? Rw.Right
                               : fit(Rw.Right, Col) + " ; " + Rw.Note)
           << "\n";
        if (C != raw_ostream::SAVEDCOLOR)
          P.reset();
      }
      OS << std::string(std::min(R.Width, 200u), '-') << "\n";
    }
  };
  std::vector<Row> Mod;
  moduleRows(D, O, Mod);
  Emit("module", Mod);
  for (auto &F : D.Funcs)
    if (funcHasKept(F, O))
      Emit(F.NameA.empty() ? F.NameB : F.NameA, F.Rows);
}

void jChange(json::OStream &J, const Change &C) {
  J.object([&] {
    J.attribute("category", C.Cat);
    J.attribute("kind", C.Kind);
    J.attribute("significance", sigName(C.Significance));
    if (!C.Func.empty())
      J.attribute("function", C.Func);
    if (!C.Block.empty())
      J.attribute("block", C.Block);
    J.attribute("subject", C.Subject);
    if (!C.Before.empty())
      J.attribute("before", C.Before);
    if (!C.After.empty())
      J.attribute("after", C.After);
    if (!C.Details.empty())
      J.attributeArray("details", [&] {
        for (auto &D : C.Details)
          J.value(D);
      });
    J.attribute("inferred", C.Inferred);
    if (!C.Confidence.empty())
      J.attribute("confidence", C.Confidence);
    if (!C.Pass.empty())
      J.attribute("pass", C.Pass);
  });
}

void jInput(json::OStream &J, const InputInfo &I) {
  J.object([&] {
    J.attribute("path", I.Path);
    J.attribute("format", I.Format);
    J.attribute("producer", I.Producer);
    J.attribute("triple", I.Triple);
    J.attribute("data_layout", I.DataLayout);
    J.attribute("functions", int64_t(I.Functions));
    J.attribute("declarations", int64_t(I.Declarations));
    J.attribute("globals", int64_t(I.Globals));
    J.attribute("instructions", int64_t(I.Instructions));
  });
}

void jTool(json::OStream &J) {
  J.attributeObject("tool", [&] {
    J.attribute("name", "irdiff");
    J.attribute("version", IRDIFF_VERSION);
    J.attribute("llvm", LLVM_VERSION_STRING);
    J.attribute("schema", 1);
  });
}

void jChanges(json::OStream &J, StringRef Key, const std::vector<Change> &L,
              const Options &O, bool SkipHints = false) {
  J.attributeArray(Key, [&] {
    for (auto &C : L)
      if (kept(C, O) && !(SkipHints && C.Cat == "hint"))
        jChange(J, C);
  });
}

void jDiffBody(json::OStream &J, const ModuleDiff &D, const Options &O,
               bool Rows) {
  Totals T = totals(D, O);
  J.attributeObject("summary", [&] {
    J.attribute("functions_compared", int64_t(T.Compared));
    J.attribute("identical", int64_t(T.Identical));
    J.attribute("equivalent", int64_t(T.Equivalent));
    J.attribute("modified", int64_t(T.Modified));
    J.attribute("added", int64_t(T.Added));
    J.attribute("removed", int64_t(T.Removed));
    J.attribute("changes", int64_t(T.Kept));
    J.attribute("hidden", int64_t(T.Hidden));
    J.attribute("max_significance", T.Kept ? sigName(T.Max) : "none");
    J.attributeObject("by_significance", [&] {
      J.attribute("major", int64_t(T.BySig[2]));
      J.attribute("minor", int64_t(T.BySig[1]));
      J.attribute("cosmetic", int64_t(T.BySig[0]));
    });
    J.attributeObject("by_category", [&] {
      for (auto &[K, N] : T.ByCat)
        J.attribute(K, int64_t(N));
    });
    J.attributeObject("by_kind", [&] {
      for (auto &[K, N] : T.ByKind)
        J.attribute(K, int64_t(N));
    });
  });
  jChanges(J, "module_changes", D.ModuleChanges, O, true);
  J.attributeArray("functions", [&] {
    for (auto &F : D.Funcs) {
      if (!funcHasKept(F, O) && F.Status != "identical" &&
          F.Status != "equivalent")
        continue;
      J.object([&] {
        if (!F.NameA.empty())
          J.attribute("name_a", F.NameA);
        if (!F.NameB.empty())
          J.attribute("name_b", F.NameB);
        J.attribute("status", F.Status);
        J.attribute("ambiguous", F.Ambiguous);
        J.attributeObject("stats", [&] {
          const FuncStats &S = F.Stats;
          J.attribute("blocks_a", int64_t(S.BlocksA));
          J.attribute("blocks_b", int64_t(S.BlocksB));
          J.attribute("instructions_a", int64_t(S.InstsA));
          J.attribute("instructions_b", int64_t(S.InstsB));
          J.attribute("added", int64_t(S.Added));
          J.attribute("removed", int64_t(S.Removed));
          J.attribute("modified", int64_t(S.Modified));
          J.attribute("moved", int64_t(S.Moved));
          J.attribute("unchanged", int64_t(S.Unchanged));
        });
        jChanges(J, "changes", F.Changes, O);
        if (Rows && !F.Rows.empty())
          J.attributeArray("rows", [&] {
            for (auto &Rw : F.Rows)
              J.object([&] {
                static const char *Tags[] = {"same", "removed", "added",
                                             "modified", "label"};
                J.attribute("tag", Tags[Rw.T]);
                if (!Rw.Left.empty())
                  J.attribute("a", Rw.Left);
                if (!Rw.Right.empty())
                  J.attribute("b", Rw.Right);
                if (!Rw.Note.empty())
                  J.attribute("note", Rw.Note);
              });
          });
      });
    }
  });
  jChanges(J, "remarks", D.RemarkChanges, O);
  jChanges(J, "pipeline", D.PassChanges, O);
  J.attributeArray("hints", [&] {
    for (auto &C : D.ModuleChanges)
      if (C.Cat == "hint" && kept(C, O))
        jChange(J, C);
  });
}

void jRemark(json::OStream &J, const Remark &R) {
  J.object([&] {
    J.attribute("type", R.Type);
    J.attribute("pass", R.Pass);
    J.attribute("name", R.Name);
    J.attribute("function", R.Function);
    if (!R.Loc.empty())
      J.attribute("location", R.Loc);
    J.attribute("message", R.Message);
    if (!R.Origin.empty())
      J.attribute("emitted_during", R.Origin);
  });
}

} // namespace

void printDiff(raw_ostream &OS, const ModuleDiff &D, const Options &O,
               const ReportOptions &R) {
  Painter P{OS, R.Color};
  switch (R.F) {
  case Format::Text:
    printText(P, D, O, R);
    return;
  case Format::Unified:
    printUnified(P, D, O, R);
    return;
  case Format::Side:
    printSide(P, D, O, R);
    return;
  case Format::Json: {
    json::OStream J(OS, 2);
    J.object([&] {
      jTool(J);
      if (!R.Command.empty())
        J.attribute("command", R.Command);
      J.attributeObject("inputs", [&] {
        J.attributeBegin("a");
        jInput(J, D.A);
        J.attributeEnd();
        J.attributeBegin("b");
        jInput(J, D.B);
        J.attributeEnd();
      });
      J.attributeObject("compatibility", [&] {
        J.attribute("incompatible", D.Incompatible);
        J.attributeArray("notes", [&] {
          for (auto &N : D.Compat)
            J.value(N);
        });
      });
      jDiffBody(J, D, O, R.Verbose);
    });
    OS << "\n";
    return;
  }
  }
}

bool stepKept(const PassStep &S, const Options &O) {
  if (!O.Passes.empty() && !matchesAny(O.Passes, S.Run.Pass) &&
      !matchesAny(O.Passes, S.Run.Short))
    return false;
  for (auto &C : S.Diff.ModuleChanges)
    if (kept(C, O))
      return true;
  for (auto &F : S.Diff.Funcs)
    if (funcHasKept(F, O))
      return true;
  return false;
}

static std::string stepSummary(const PassStep &S, const Options &O) {
  std::map<std::string, std::map<std::string, unsigned>> N;
  auto Add = [&](const Change &C) {
    if (kept(C, O))
      ++N[C.Cat][C.Kind];
  };
  for (auto &C : S.Diff.ModuleChanges)
    Add(C);
  for (auto &F : S.Diff.Funcs)
    for (auto &C : F.Changes)
      Add(C);
  std::string R;
  for (auto &[Cat, Kinds] : N) {
    std::string Part = Cat;
    for (auto &[K, V] : Kinds) {
      if (K == "added")
        Part += " +" + std::to_string(V);
      else if (K == "removed")
        Part += " -" + std::to_string(V);
      else
        Part += " " + K + "=" + std::to_string(V);
    }
    R += (R.empty() ? "" : "; ") + Part;
  }
  if (!S.Remarks.empty())
    R += (R.empty() ? "" : "; ") + std::to_string(S.Remarks.size()) +
         " remark(s)";
  return R;
}

void printPasses(raw_ostream &OS, const InputInfo &In,
                 const std::string &Pipeline,
                 const std::vector<PassStep> &Steps, const PipelineResult *PR,
                 const std::vector<std::string> &Notes, const Options &O,
                 const ReportOptions &R) {
  Painter P{OS, R.Color};
  unsigned Shown = 0;
  for (auto &S : Steps)
    Shown += stepKept(S, O);
  if (R.F == Format::Json) {
    json::OStream J(OS, 2);
    J.object([&] {
      jTool(J);
      J.attributeBegin("input");
      jInput(J, In);
      J.attributeEnd();
      J.attribute("pipeline", Pipeline);
      if (PR)
        J.attribute("pipeline_expanded", PR->PipelineText);
      J.attributeArray("notes", [&] {
        for (auto &N : Notes)
          J.value(N);
      });
      if (PR)
        J.attributeArray("executions", [&] {
          for (auto &Run : PR->Runs)
            J.object([&] {
              J.attribute("index", int64_t(Run.Index));
              J.attribute("pass", Run.Pass);
              if (!Run.Short.empty())
                J.attribute("name", Run.Short);
              J.attribute("unit", Run.Unit);
              J.attribute("changed", Run.Changed);
            });
        });
      J.attributeArray("steps", [&] {
        for (auto &S : Steps) {
          if (!stepKept(S, O))
            continue;
          J.object([&] {
            J.attribute("index", int64_t(S.Run.Index));
            J.attribute("pass", S.Run.Pass);
            if (!S.Run.Short.empty())
              J.attribute("name", S.Run.Short);
            J.attribute("unit", S.Run.Unit);
            J.attributeArray("changed_functions", [&] {
              for (auto &F : S.Functions)
                J.value(F);
            });
            jDiffBody(J, S.Diff, O, R.Verbose);
            J.attributeArray("emitted_remarks", [&] {
              for (auto &Rm : S.Remarks)
                jRemark(J, Rm);
            });
          });
        }
      });
      if (PR)
        J.attributeArray("remarks", [&] {
          for (auto &Rm : PR->Remarks)
            if (O.Functions.empty() || matchesAny(O.Functions, Rm.Function))
              jRemark(J, Rm);
        });
    });
    OS << "\n";
    return;
  }
  P.bold();
  OS << "irdiff passes";
  P.reset();
  OS << ": " << In.Path;
  if (!Pipeline.empty())
    OS << "  pipeline: " << Pipeline;
  OS << "\n";
  for (auto &N : Notes) {
    P.col(raw_ostream::MAGENTA);
    OS << "note: ";
    P.reset();
    OS << N << "\n";
  }
  if (PR) {
    unsigned Ch = 0;
    for (auto &Run : PR->Runs)
      Ch += Run.Changed;
    OS << PR->Runs.size() << " pass execution(s), " << Ch << " changed the IR, "
       << Shown << " shown after filters\n";
  } else {
    OS << Steps.size() << " changing dump(s), " << Shown
       << " shown after filters\n";
  }
  for (auto &S : Steps) {
    if (!stepKept(S, O))
      continue;
    OS << "\n";
    P.col(raw_ostream::CYAN, true);
    OS << "#" << S.Run.Index << " " << S.Run.Pass;
    P.reset();
    if (!S.Run.Short.empty())
      OS << " (" << S.Run.Short << ")";
    if (!S.Run.Unit.empty())
      OS << " on " << S.Run.Unit;
    OS << ": " << stepSummary(S, O) << "\n";
    if (R.F == Format::Unified || R.F == Format::Side) {
      ReportOptions Sub = R;
      if (R.F == Format::Unified)
        printUnified(P, S.Diff, O, Sub);
      else
        printSide(P, S.Diff, O, Sub);
      continue;
    }
    if (!R.Verbose && R.Summary)
      continue;
    for (auto &C : S.Diff.ModuleChanges)
      if (kept(C, O))
        printChange(P, C, R, "    ");
    for (auto &F : S.Diff.Funcs) {
      if (!funcHasKept(F, O))
        continue;
      OS << "  " << (F.NameA.empty() ? F.NameB : F.NameA) << " " << F.Status
         << "\n";
      for (auto &C : F.Changes)
        if (kept(C, O))
          printChange(P, C, R, "    ");
    }
    for (auto &Rm : S.Remarks)
      if (O.Functions.empty() || matchesAny(O.Functions, Rm.Function))
        OS << "    remark [" << Rm.Type << "] " << Rm.Pass << "/" << Rm.Name
           << " in @" << Rm.Function << ": " << Rm.Message << "\n";
  }
}

void printAnalyze(raw_ostream &OS, const InputInfo &In,
                  const std::vector<FuncDiff> &Fs, const Options &O,
                  const ReportOptions &R) {
  if (R.F == Format::Json) {
    json::OStream J(OS, 2);
    J.object([&] {
      jTool(J);
      J.attributeBegin("input");
      jInput(J, In);
      J.attributeEnd();
      J.attributeArray("functions", [&] {
        for (auto &F : Fs)
          J.object([&] {
            J.attribute("name", F.NameA);
            J.attribute("blocks", int64_t(F.Stats.BlocksA));
            J.attribute("instructions", int64_t(F.Stats.InstsA));
            J.attributeArray("facts", [&] {
              for (auto &C : F.Changes)
                jChange(J, C);
            });
          });
      });
    });
    OS << "\n";
    return;
  }
  Painter P{OS, R.Color};
  OS << "irdiff analyze: " << inputLine(In) << "\n";
  ReportOptions Sub = R;
  for (auto &F : Fs) {
    OS << "\n";
    P.bold();
    OS << F.NameA;
    P.reset();
    OS << "  (" << F.Stats.BlocksA << " blocks, " << F.Stats.InstsA
       << " instructions)\n";
    for (auto &C : F.Changes)
      printChange(P, C, Sub, "  ");
  }
}

} // namespace irdiff
