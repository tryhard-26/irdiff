#include "Analyses.h"
#include "Pipeline.h"
#include "Report.h"
#include "Version.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/WithColor.h"
#include "llvm/TargetParser/Host.h"
#include <cstdlib>
#include <cstring>

using namespace llvm;
using namespace irdiff;

static cl::OptionCategory OutCat("Output options");
static cl::OptionCategory FilterCat("Filtering options");
static cl::OptionCategory NormCat("Normalization options");
static cl::OptionCategory InCat("Input and pipeline options");

static cl::SubCommand DiffCmd("diff", "Compare two IR snapshots (.ll or .bc)");
static cl::SubCommand
    PassesCmd("passes",
              "Run a pass pipeline and attribute each IR change to a pass");
static cl::SubCommand
    PipelinesCmd("pipelines",
                 "Run two pipelines/configurations on one input and compare");
static cl::SubCommand
    AnalyzeCmd("analyze",
               "Report dominator, loop, SCEV, alias and MemorySSA facts");
static cl::SubCommand
    InvestigateCmd("investigate",
                   "Diff with analyses, remarks and evidence-backed hints");

static cl::list<std::string> Inputs(cl::Positional, cl::desc("<inputs>"),
                                    cl::sub(DiffCmd), cl::sub(PassesCmd),
                                    cl::sub(PipelinesCmd), cl::sub(AnalyzeCmd),
                                    cl::sub(InvestigateCmd));

#define ALL cl::sub(cl::SubCommand::getAll())

static cl::opt<Format> FormatOpt(
    "format", cl::desc("Output format"), cl::init(Format::Text),
    cl::values(clEnumValN(Format::Text, "text", "readable report (default)"),
               clEnumValN(Format::Unified, "unified", "unified diff of IR"),
               clEnumValN(Format::Side, "side-by-side", "two-column diff"),
               clEnumValN(Format::Json, "json", "machine-readable JSON")),
    cl::cat(OutCat), ALL);
static cl::alias FormatShort("f", cl::aliasopt(FormatOpt),
                             cl::desc("Alias for --format"), cl::cat(OutCat));
static cl::opt<std::string> OutFile("o", cl::desc("Write output to <file>"),
                                    cl::value_desc("file"), cl::cat(OutCat),
                                    ALL);
static cl::opt<bool>
    Verbose("verbose",
            cl::desc("Show all details, significance and full listings"),
            cl::cat(OutCat), ALL);
static cl::alias VerboseShort("v", cl::aliasopt(Verbose),
                              cl::desc("Alias for --verbose"), cl::cat(OutCat));
static cl::opt<bool> Quiet("quiet",
                           cl::desc("Print nothing; only set the exit code"),
                           cl::cat(OutCat), ALL);
static cl::alias QuietShort("q", cl::aliasopt(Quiet),
                            cl::desc("Alias for --quiet"), cl::cat(OutCat));
static cl::opt<bool> SummaryOnly("summary",
                                 cl::desc("Print only summary statistics"),
                                 cl::cat(OutCat), ALL);
static cl::opt<unsigned>
    ContextLines("context",
                 cl::desc("Context lines for unified/side-by-side output"),
                 cl::init(3), cl::cat(OutCat), ALL);
static cl::alias ContextShort("U", cl::aliasopt(ContextLines),
                              cl::desc("Alias for --context"), cl::cat(OutCat));
static cl::opt<unsigned> Width(
    "width",
    cl::desc("Total width for side-by-side output (default: $COLUMNS or 160)"),
    cl::init(0), cl::cat(OutCat), ALL);
static cl::opt<Names> NamesOpt(
    "names", cl::desc("Value names in listings"), cl::init(Names::Original),
    cl::values(clEnumValN(Names::Original, "original",
                          "names from the inputs (default)"),
               clEnumValN(Names::Canonical, "canonical",
                          "normalized names shared by matched values")),
    cl::cat(OutCat), ALL);
static cl::opt<std::string> FailOn(
    "fail-on",
    cl::desc(
        "Exit 1 when shown changes reach this level: any|minor|major|never"),
    cl::init("any"), cl::cat(OutCat), ALL);
static cl::opt<bool> Hints(
    "hints",
    cl::desc(
        "Add evidence-backed pointers for missed-optimization investigation"),
    cl::cat(OutCat), ALL);

static cl::list<std::string>
    FuncFilter("function",
               cl::desc("Only functions matching glob (repeatable)"),
               cl::cat(FilterCat), ALL);
static cl::alias FuncShort("F", cl::aliasopt(FuncFilter),
                           cl::desc("Alias for --function"),
                           cl::cat(FilterCat));
static cl::list<std::string>
    BlockFilter("block", cl::desc("Only changes in blocks matching glob"),
                cl::cat(FilterCat), ALL);
static cl::list<std::string>
    CatFilter("category", cl::CommaSeparated,
              cl::desc("Only these change categories (comma-separated; see "
                       "--list-categories)"),
              cl::cat(FilterCat), ALL);
static cl::list<std::string> KindFilter(
    "change", cl::CommaSeparated,
    cl::desc(
        "Only these change kinds (comma-separated; see --list-categories)"),
    cl::cat(FilterCat), ALL);
static cl::list<std::string>
    PassFilter("pass",
               cl::desc("Only passes matching glob (class or pipeline name)"),
               cl::cat(FilterCat), ALL);
static cl::list<std::string> AnalysisFilter(
    "analysis", cl::CommaSeparated,
    cl::desc("Analyses to run/report: dom,loops,scev,aa,mssa (default: all)"),
    cl::cat(FilterCat), ALL);
static cl::opt<std::string>
    MinSig("min-significance",
           cl::desc("Hide changes below cosmetic|minor|major (default: minor)"),
           cl::init("minor"), cl::cat(FilterCat), ALL);
static cl::opt<bool> All(
    "all",
    cl::desc("Show cosmetic changes too (same as --min-significance=cosmetic)"),
    cl::cat(FilterCat), ALL);
static cl::opt<bool>
    MajorOnly("major", cl::desc("Show only potentially significant changes"),
              cl::cat(FilterCat), ALL);
static cl::opt<bool>
    ListCats("list-categories",
             cl::desc("List categories, kinds and analyses, then exit"),
             cl::cat(FilterCat), ALL);

static cl::opt<bool>
    DebugInfo("debug-info",
              cl::desc("Compare debug locations and variable records"),
              cl::cat(NormCat), ALL);
static cl::opt<bool>
    NoMetadata("ignore-metadata",
               cl::desc("Ignore non-debug metadata attachments"),
               cl::cat(NormCat), ALL);
static cl::opt<bool>
    NoAttrs("ignore-attributes",
            cl::desc("Ignore function and call-site attribute differences"),
            cl::cat(NormCat), ALL);
static cl::opt<bool> Strict(
    "strict",
    cl::desc("Report layout, label and source_filename changes as minor"),
    cl::cat(NormCat), ALL);

static cl::opt<bool> NoVerify("no-verify",
                              cl::desc("Skip the IR verifier on inputs"),
                              cl::cat(InCat), ALL);
static cl::opt<bool> RequireCompat(
    "require-compatible",
    cl::desc("Exit 4 if the inputs target different triples or data layouts"),
    cl::cat(InCat), ALL);
static cl::opt<bool> Analyses("analyses",
                              cl::desc("diff: also compare dominators, loops, "
                                       "SCEV, alias analysis and MemorySSA"),
                              cl::cat(InCat), ALL);
static cl::opt<unsigned>
    AALimit("aa-limit",
            cl::desc("Max loads/stores per function for alias queries"),
            cl::init(48), cl::cat(InCat), ALL);
static cl::opt<std::string>
    RemarksA("remarks-a",
             cl::desc("Optimization remarks (YAML or bitstream) for A"),
             cl::value_desc("file"), cl::cat(InCat), ALL);
static cl::opt<std::string>
    RemarksB("remarks-b",
             cl::desc("Optimization remarks (YAML or bitstream) for B"),
             cl::value_desc("file"), cl::cat(InCat), ALL);
static cl::opt<std::string> PipelineOpt(
    "p", cl::desc("Pass pipeline (opt -passes syntax), e.g. 'default<O2>'"),
    cl::value_desc("pipeline"), cl::cat(InCat), ALL);
static cl::opt<std::string>
    OptLevel("O", cl::Prefix,
             cl::desc("Shorthand for -p 'default<ON>' (0,1,2,3,s,z)"),
             cl::cat(InCat), ALL);
static cl::opt<std::string> PipeA("a",
                                  cl::desc("pipelines: pipeline for side A"),
                                  cl::init("default<O1>"), cl::cat(InCat),
                                  cl::sub(PipelinesCmd));
static cl::opt<std::string> PipeB("b",
                                  cl::desc("pipelines: pipeline for side B"),
                                  cl::init("default<O2>"), cl::cat(InCat),
                                  cl::sub(PipelinesCmd));
static cl::opt<std::string> CPU("mcpu",
                                cl::desc("Target CPU for the pass pipeline"),
                                cl::cat(InCat), ALL);
static cl::opt<std::string>
    Attrs("mattr", cl::desc("Target features for the pass pipeline"),
          cl::cat(InCat), ALL);
static cl::opt<std::string>
    CPUB("mcpu-b",
         cl::desc("pipelines: target CPU for side B (default: --mcpu)"),
         cl::cat(InCat), cl::sub(PipelinesCmd));
static cl::opt<std::string>
    AttrsB("mattr-b",
           cl::desc("pipelines: target features for side B (default: --mattr)"),
           cl::cat(InCat), cl::sub(PipelinesCmd));
static cl::opt<bool>
    NoTarget("no-target",
             cl::desc("Do not create a TargetMachine (generic cost models)"),
             cl::cat(InCat), ALL);
static cl::opt<bool>
    VerifyEachPass("verify-each-pass",
                   cl::desc("Run the verifier after every pass"),
                   cl::cat(InCat), ALL);
static cl::opt<std::string>
    SaveDir("save-snapshots",
            cl::desc("passes: write the IR after every changing pass to <dir>"),
            cl::value_desc("dir"), cl::cat(InCat), ALL);
static cl::opt<std::string>
    FromLog("from-log",
            cl::desc("passes: replay a -print-after-all/-print-changed log "
                     "with -print-module-scope"),
            cl::value_desc("file"), cl::cat(InCat), ALL);

static int ColorChoice = 0;

static void printVersion(raw_ostream &OS) {
  OS << "irdiff " << IRDIFF_VERSION;
  if (StringRef(IRDIFF_GIT).size())
    OS << " (" << IRDIFF_GIT << ")";
  OS << "\n  LLVM " << LLVM_VERSION_STRING << " (supported: 19-23)\n"
     << "  default target: " << sys::getDefaultTargetTriple() << "\n";
}

static void fail(const std::string &Msg) {
  WithColor::error(errs(), "irdiff") << Msg << "\n";
}

static bool validList(const cl::list<std::string> &L,
                      const std::vector<std::string> &Allowed, StringRef What) {
  for (auto &V : L)
    if (std::find(Allowed.begin(), Allowed.end(), V) == Allowed.end()) {
      std::string S;
      for (auto &A : Allowed)
        S += (S.empty() ? "" : ", ") + A;
      fail("unknown " + What.str() + " '" + V + "' (expected one of: " + S +
           ")");
      return false;
    }
  return true;
}

static bool buildOptions(DiffOptions &O) {
  if (!validList(CatFilter, AllCategories, "category") ||
      !validList(KindFilter, AllKinds, "change kind") ||
      !validList(AnalysisFilter, AllAnalyses, "analysis"))
    return false;
  O.Functions.assign(FuncFilter.begin(), FuncFilter.end());
  O.Blocks.assign(BlockFilter.begin(), BlockFilter.end());
  O.Categories.assign(CatFilter.begin(), CatFilter.end());
  O.Kinds.assign(KindFilter.begin(), KindFilter.end());
  O.Passes.assign(PassFilter.begin(), PassFilter.end());
  O.Analyses.assign(AnalysisFilter.begin(), AnalysisFilter.end());
  if (MinSig == "cosmetic")
    O.MinSig = Sig::Cosmetic;
  else if (MinSig == "minor")
    O.MinSig = Sig::Minor;
  else if (MinSig == "major")
    O.MinSig = Sig::Major;
  else {
    fail("--min-significance must be cosmetic, minor or major");
    return false;
  }
  if (All)
    O.MinSig = Sig::Cosmetic;
  if (MajorOnly)
    O.MinSig = Sig::Major;
  O.CompareDebug = DebugInfo;
  O.CompareMetadata = !NoMetadata;
  O.CompareAttributes = !NoAttrs;
  O.Strict = Strict;
  O.IgnoreLayout = !Strict;
  O.Verify = !NoVerify;
  O.AALimit = AALimit;
  O.NameMode = NamesOpt;
  O.Rows = FormatOpt == Format::Unified || FormatOpt == Format::Side ||
           (FormatOpt == Format::Json && Verbose);
  if (FailOn != "any" && FailOn != "minor" && FailOn != "major" &&
      FailOn != "never") {
    fail("--fail-on must be any, minor, major or never");
    return false;
  }
  return true;
}

static int exitFor(unsigned Kept, Sig Max) {
  if (FailOn == "never" || Kept == 0)
    return ExitSame;
  if (FailOn == "major" && Max < Sig::Major)
    return ExitSame;
  if (FailOn == "minor" && Max < Sig::Minor)
    return ExitSame;
  return ExitDiffer;
}

static std::unique_ptr<raw_fd_ostream> FileOut;

static raw_ostream &out() {
  if (Quiet)
    return nulls();
  if (!OutFile.empty() && OutFile != "-") {
    if (!FileOut) {
      std::error_code EC;
      FileOut = std::make_unique<raw_fd_ostream>(OutFile, EC, sys::fs::OF_Text);
      if (EC) {
        fail(OutFile + ": " + EC.message());
        std::exit(ExitInput);
      }
    }
    return *FileOut;
  }
  return outs();
}

static ReportOptions reportOptions(const std::string &Cmd) {
  ReportOptions R;
  R.F = FormatOpt;
  R.Verbose = Verbose;
  R.Summary = SummaryOnly;
  R.Context = ContextLines;
  R.Command = Cmd;
  unsigned W = Width;
  if (!W)
    if (const char *C = std::getenv("COLUMNS"))
      W = std::atoi(C);
  R.Width = W ? W : 160;
  bool Tty =
      OutFile.empty() && !Quiet && sys::Process::StandardOutIsDisplayed();
  bool Env = !std::getenv("NO_COLOR") &&
             !(std::getenv("TERM") && StringRef(std::getenv("TERM")) == "dumb");
  R.Color = FormatOpt != Format::Json &&
            (ColorChoice == 1 || (ColorChoice == 0 && Tty && Env));
  return R;
}

static std::unique_ptr<Loaded> load(const std::string &Path, int &Code) {
  LoadResult R = loadModule(Path, !NoVerify);
  if (!R.L) {
    fail(R.Error);
    Code = R.Code;
    return nullptr;
  }
  return std::move(R.L);
}

static std::string pipelineText() {
  if (!PipelineOpt.empty())
    return PipelineOpt;
  if (!OptLevel.empty())
    return "default<O" + OptLevel + ">";
  return "default<O2>";
}

static int runDiff(bool Investigate) {
  if (Inputs.size() != 2) {
    fail(std::string(Investigate ? "investigate" : "diff") +
         " expects exactly two inputs: <A> <B>");
    return ExitUsage;
  }
  if (Inputs[0] == "-" && Inputs[1] == "-") {
    fail("only one input can be read from stdin");
    return ExitUsage;
  }
  DiffOptions O;
  if (!buildOptions(O))
    return ExitUsage;
  O.RunAnalyses = Analyses || Investigate;
  int Code = 0;
  auto A = load(Inputs[0], Code);
  if (!A)
    return Code;
  auto B = load(Inputs[1], Code);
  if (!B)
    return Code;
  if (RequireCompat) {
    ModuleDiff Probe;
    Probe.A = A->Info;
    Probe.B = B->Info;
    checkCompat(Probe);
    if (Probe.Incompatible) {
      for (auto &N : Probe.Compat)
        fail(N);
      return ExitIncompatible;
    }
  }
  ModuleDiff D = diffModules(*A, *B, O);
  if (!RemarksA.empty() || !RemarksB.empty()) {
    std::vector<Remark> RA, RB;
    std::string Err;
    if ((!RemarksA.empty() && !loadRemarks(RemarksA, RA, Err)) ||
        (!RemarksB.empty() && !loadRemarks(RemarksB, RB, Err))) {
      fail(Err);
      return ExitInput;
    }
    diffRemarks(RA, RB, D, O);
  }
  if (Hints || Investigate)
    addHints(D);
  printDiff(out(), D, O, reportOptions(Investigate ? "investigate" : "diff"));
  Totals T = totals(D, O);
  return exitFor(T.Kept, T.Max);
}

static PipelineConfig config(bool SideB) {
  PipelineConfig C;
  C.Passes = pipelineText();
  C.CPU = SideB && !CPUB.empty() ? CPUB : CPU;
  C.Features = SideB && !AttrsB.empty() ? AttrsB : Attrs;
  C.UseTarget = !NoTarget;
  C.VerifyEach = VerifyEachPass;
  return C;
}

static int runPasses() {
  DiffOptions O;
  if (!buildOptions(O))
    return ExitUsage;
  std::vector<PassStep> Steps;
  std::string Err;
  if (!FromLog.empty()) {
    std::vector<std::string> Notes;
    if (!traceLog(FromLog, O, Steps, Notes, Err)) {
      fail(Err);
      return ExitInput;
    }
    InputInfo In;
    In.Path = FromLog;
    In.Format = "pass log";
    printPasses(out(), In, "", Steps, nullptr, Notes, O,
                reportOptions("passes"));
  } else {
    if (Inputs.size() != 1) {
      fail("passes expects one input (or --from-log <file>)");
      return ExitUsage;
    }
    int Code = 0;
    auto L = load(Inputs[0], Code);
    if (!L)
      return Code;
    PipelineResult R;
    InputInfo In = L->Info;
    if (!tracePasses(*L, config(false), O, SaveDir, Steps, R, Err)) {
      fail(Err);
      return Err.rfind("invalid pass pipeline", 0) == 0 ? ExitUsage
                                                        : ExitInternal;
    }
    printPasses(out(), In, pipelineText(), Steps, &R, R.Notes, O,
                reportOptions("passes"));
  }
  unsigned Kept = 0;
  for (auto &S : Steps)
    Kept += stepKept(S, O);
  return exitFor(Kept, Sig::Major);
}

static int runPipelines() {
  if (Inputs.size() != 1) {
    fail("pipelines expects one input");
    return ExitUsage;
  }
  DiffOptions O;
  if (!buildOptions(O))
    return ExitUsage;
  O.RunAnalyses = Analyses;
  int Code = 0;
  auto A = load(Inputs[0], Code);
  if (!A)
    return Code;
  auto B = load(Inputs[0], Code);
  if (!B)
    return Code;
  PipelineConfig CA = config(false), CB = config(true);
  CA.Passes = PipeA;
  CB.Passes = PipeB;
  PipelineResult RA, RB;
  std::string Err;
  if (!runPipeline(*A->M, CA, RA, Err) || !runPipeline(*B->M, CB, RB, Err)) {
    fail(Err);
    return Err.rfind("invalid pass pipeline", 0) == 0 ? ExitUsage
                                                      : ExitInternal;
  }
  fillInfo(*A);
  fillInfo(*B);
  A->Info.Path = Inputs[0] + " [" + CA.Passes +
                 (CA.CPU.empty() ? "" : ", cpu=" + CA.CPU) +
                 (CA.Features.empty() ? "" : ", attrs=" + CA.Features) + "]";
  B->Info.Path = Inputs[0] + " [" + CB.Passes +
                 (CB.CPU.empty() ? "" : ", cpu=" + CB.CPU) +
                 (CB.Features.empty() ? "" : ", attrs=" + CB.Features) + "]";
  ModuleDiff D = diffModules(*A, *B, O);
  for (auto &N : RA.Notes)
    D.Compat.push_back("A: " + N);
  for (auto &N : RB.Notes)
    D.Compat.push_back("B: " + N);
  diffRemarks(RA.Remarks, RB.Remarks, D, O);
  diffPipelines(RA, RB, D);
  if (Hints)
    addHints(D);
  printDiff(out(), D, O, reportOptions("pipelines"));
  Totals T = totals(D, O);
  return exitFor(T.Kept, T.Max);
}

static int runAnalyze() {
  if (Inputs.size() != 1) {
    fail("analyze expects one input");
    return ExitUsage;
  }
  DiffOptions O;
  if (!buildOptions(O))
    return ExitUsage;
  int Code = 0;
  auto L = load(Inputs[0], Code);
  if (!L)
    return Code;
  InputInfo In = L->Info;
  auto Fs = analyzeModule(*L, O);
  printAnalyze(out(), In, Fs, O, reportOptions("analyze"));
  return ExitSame;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  std::vector<const char *> Args;
  Args.push_back(argv[0]);
  static const char *Subs[] = {"diff", "passes", "pipelines", "analyze",
                               "investigate"};
  bool HaveSub = false;
  for (int I = 1; I < argc; ++I) {
    StringRef A = argv[I];
    if (A == "--color" || A == "--color=always" || A == "--colour") {
      ColorChoice = 1;
      continue;
    }
    if (A == "--no-color" || A == "--color=never") {
      ColorChoice = 2;
      continue;
    }
    if (A == "--color=auto") {
      ColorChoice = 0;
      continue;
    }
    if (A.starts_with("--color=")) {
      fail("--color must be auto, always or never");
      return ExitUsage;
    }
    if (I == 1)
      for (const char *S : Subs)
        HaveSub |= A == S;
    if (I == 1 && !HaveSub && !A.starts_with("-"))
      Args.push_back("diff");
    Args.push_back(argv[I]);
  }
  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions({&OutCat, &FilterCat, &NormCat, &InCat});
  std::string Err;
  raw_string_ostream ES(Err);
  if (!cl::ParseCommandLineOptions(
          Args.size(), Args.data(),
          "irdiff: compare LLVM IR and explain optimization changes\n\n"
          "  irdiff [diff] A.ll B.ll          compare two snapshots\n"
          "  irdiff passes in.ll -O2          attribute changes to passes\n"
          "  irdiff passes --from-log log.txt replay -print-after-all output\n"
          "  irdiff pipelines in.ll -a P -b Q compare two pipelines\n"
          "  irdiff analyze in.ll             report analysis facts\n"
          "  irdiff investigate A.ll B.ll     diff + analyses + hints\n\n"
          "  --color[=auto|always|never]  colorize text output (honours "
          "NO_COLOR)\n\n"
          "Exit status: 0 no differences, 1 differences, 2 usage error,\n"
          "             3 unreadable/invalid input, 4 incompatible input,\n"
          "             5 internal or pipeline failure\n",
          &ES)) {
    errs() << ES.str();
    errs() << "Run 'irdiff --help' for usage.\n";
    return ExitUsage;
  }
  if (ListCats) {
    outs() << "categories:";
    for (auto &C : AllCategories)
      outs() << " " << C;
    outs() << "\nkinds:";
    for (auto &C : AllKinds)
      outs() << " " << C;
    outs() << "\nanalyses:";
    for (auto &C : AllAnalyses)
      outs() << " " << C;
    outs() << "\n";
    return ExitSame;
  }
  int Code;
  if (DiffCmd)
    Code = runDiff(false);
  else if (InvestigateCmd)
    Code = runDiff(true);
  else if (PassesCmd)
    Code = runPasses();
  else if (PipelinesCmd)
    Code = runPipelines();
  else if (AnalyzeCmd)
    Code = runAnalyze();
  else {
    fail("missing command or inputs");
    errs() << "Run 'irdiff --help' for usage.\n";
    return ExitUsage;
  }
  if (FileOut)
    FileOut->flush();
  outs().flush();
  return Code;
}
