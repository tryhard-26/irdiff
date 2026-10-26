#include "Pipeline.h"
#include "Analyses.h"
#include "Test.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace irdiff;

static std::unique_ptr<Loaded> load(const std::string &F) {
  LoadResult R = loadModule(t::fixture(F), true);
  if (!R.L)
    t::fail(__FILE__, __LINE__, R.Error);
  return std::move(R.L);
}

static unsigned allocas(llvm::Module &M) {
  unsigned N = 0;
  for (auto &F : M)
    for (auto &B : F)
      for (auto &I : B)
        N += llvm::isa<llvm::AllocaInst>(I);
  return N;
}

TEST(pipeline, run_mem2reg) {
  auto L = load("real/kernels.O0.ll");
  CHECK(allocas(*L->M) > 0);
  PipelineConfig C;
  C.Passes = "function(mem2reg)";
  PipelineResult R;
  std::string Err;
  CHECK_MSG(runPipeline(*L->M, C, R, Err), Err);
  CHECK_EQ(allocas(*L->M), 0u);
  bool Ran = false;
  for (auto &Run : R.Runs)
    Ran |= Run.Pass == "PromotePass" && Run.Changed && Run.Short == "mem2reg";
  CHECK(Ran);
  CHECK(R.PipelineText.find("mem2reg") != std::string::npos);
}

TEST(pipeline, invalid_pipeline) {
  auto L = load("loops.ll");
  PipelineConfig C;
  C.Passes = "no-such-pass";
  PipelineResult R;
  std::string Err;
  CHECK(!runPipeline(*L->M, C, R, Err));
  CHECK_MSG(Err.find("invalid pass pipeline") != std::string::npos, Err);
}

TEST(pipeline, no_target_note) {
  auto L = load("loops.ll");
  PipelineConfig C;
  C.Passes = "function(instcombine)";
  C.UseTarget = false;
  PipelineResult R;
  std::string Err;
  CHECK(runPipeline(*L->M, C, R, Err));
  CHECK(!R.Notes.empty());
  CHECK(R.Notes[0].find("disabled") != std::string::npos);
  auto L2 = load("loops.ll");
  PipelineResult R2;
  C.UseTarget = true;
  CHECK(runPipeline(*L2->M, C, R2, Err));
  CHECK(!R2.Notes.empty());
  CHECK(R2.Notes[0].find("no target triple") != std::string::npos);
}

TEST(pipeline, trace_attributes_changes_to_passes) {
  auto L = load("real/kernels.O0.ll");
  PipelineConfig C;
  C.Passes = "function(sroa,instcombine,simplifycfg)";
  DiffOptions O;
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK_MSG(tracePasses(*L, C, O, "", Steps, R, Err), Err);
  CHECK(!Steps.empty());
  bool SroaRemovedAlloca = false;
  for (auto &S : Steps) {
    CHECK(S.Run.Changed);
    CHECK(!S.Functions.empty());
    if (S.Run.Pass == "SROAPass")
      SroaRemovedAlloca |=
          t::find(S.Diff, "instruction", "removed", "alloca") != nullptr;
  }
  CHECK(SroaRemovedAlloca);
  unsigned Changed = 0;
  for (auto &Run : R.Runs)
    Changed += Run.Changed;
  CHECK_EQ(Changed, unsigned(Steps.size()));
}

TEST(pipeline, trace_function_filter) {
  auto L = load("real/kernels.O0.ll");
  PipelineConfig C;
  C.Passes = "function(sroa)";
  DiffOptions O;
  O.Functions = {"classify"};
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK(tracePasses(*L, C, O, "", Steps, R, Err));
  CHECK_EQ(Steps.size(), 1u);
  CHECK_EQ(Steps[0].Run.Unit, std::string("@classify"));
}

TEST(pipeline, trace_saves_parseable_snapshots) {
  auto L = load("real/kernels.O0.ll");
  PipelineConfig C;
  C.Passes = "function(sroa,early-cse)";
  std::string Dir = t::scratch("snaps");
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK(tracePasses(*L, C, DiffOptions(), Dir, Steps, R, Err));
  std::error_code EC;
  unsigned Files = 0;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC)) {
    ++Files;
    LoadResult S = loadModule(It->path(), true);
    CHECK_MSG(S.L, S.Error);
  }
  CHECK_EQ(Files, unsigned(Steps.size()) + 1);
}

TEST(pipeline, remarks_are_attributed_to_passes) {
  auto L = load("real/kernels.O0.ll");
  PipelineConfig C;
  C.Passes = "default<O2>";
  DiffOptions O;
  O.Functions = {"scale_restrict"};
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK_MSG(tracePasses(*L, C, O, "", Steps, R, Err), Err);
  bool Hoist = false, Moved = false;
  for (auto &S : Steps)
    if (S.Run.Pass == "LICMPass") {
      for (auto &Rm : S.Remarks)
        Hoist |= Rm.Pass == "licm" && Rm.Name == "Hoisted";
      for (auto *Ch : t::changes(S.Diff))
        Moved |= Ch->Kind == "moved" && t::hasDetail(Ch, "now outside");
    }
  CHECK(Hoist);
  CHECK(Moved);
}

TEST(pipeline, compare_pipelines) {
  auto A = load("real/kernels.O0.ll");
  auto B = load("real/kernels.O0.ll");
  PipelineConfig CA, CB;
  CA.Passes = "default<O1>";
  CB.Passes = "default<O2>";
  PipelineResult RA, RB;
  std::string Err;
  CHECK(runPipeline(*A->M, CA, RA, Err));
  CHECK(runPipeline(*B->M, CB, RB, Err));
  ModuleDiff D = diffModules(*A, *B, DiffOptions());
  diffRemarks(RA.Remarks, RB.Remarks, D, Options());
  diffPipelines(RA, RB, D);
  CHECK(!D.PassChanges.empty());
  CHECK(t::find(D, "pass", "added", "loop-vectorize") ||
        t::find(D, "pass", "added", "vectorize"));
  CHECK(!D.RemarkChanges.empty());
}

TEST(pipeline, replay_print_after_all_log) {
  std::vector<PassStep> Steps;
  std::vector<std::string> Notes;
  std::string Err;
  CHECK_MSG(traceLog(t::fixture("real/kernels.passes.log"), DiffOptions(),
                     Steps, Notes, Err),
            Err);
  CHECK(!Steps.empty());
  bool Sroa = false;
  for (auto &S : Steps) {
    Sroa |= S.Run.Pass == "SROAPass";
    CHECK(S.Run.Changed);
  }
  CHECK(Sroa);
  CHECK_EQ(Notes.size(), 1u);
}

TEST(pipeline, replay_rejects_function_scoped_log) {
  std::string P = t::scratch("fn-scope.log");
  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(P, EC);
    OS << "; *** IR Dump After SROAPass on f ***\ndefine void @f() {\n  ret "
          "void\n}\n";
  }
  std::vector<PassStep> Steps;
  std::vector<std::string> Notes;
  std::string Err;
  CHECK(!traceLog(P, DiffOptions(), Steps, Notes, Err));
  CHECK_MSG(Err.find("print-module-scope") != std::string::npos, Err);
}

TEST(pipeline, replay_rejects_non_log) {
  std::vector<PassStep> Steps;
  std::vector<std::string> Notes;
  std::string Err;
  CHECK(!traceLog(t::fixture("loops.ll"), DiffOptions(), Steps, Notes, Err));
  CHECK(Err.find("no '*** IR Dump'") != std::string::npos);
}

static std::unique_ptr<Loaded> parse(const std::string &Text) {
  LoadResult R = loadModuleFromBuffer(Text, "t.ll", true);
  if (!R.L)
    t::fail(__FILE__, __LINE__, R.Error);
  return std::move(R.L);
}

TEST(pipeline, declaration_changes_are_attributed) {
  auto L = parse("target triple = \"x86_64-unknown-linux-gnu\"\n"
                 "declare i64 @strlen(ptr)\n"
                 "define i64 @f(ptr %s) {\n  %n = call i64 @strlen(ptr %s)\n"
                 "  ret i64 %n\n}\n");
  PipelineConfig C;
  C.Passes = "inferattrs";
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK_MSG(tracePasses(*L, C, DiffOptions(), "", Steps, R, Err), Err);
  CHECK_EQ(Steps.size(), 1u);
  const Change *D =
      t::find(Steps[0].Diff, "declaration", "modified", "@strlen");
  CHECK_MSG(D, t::dump(Steps[0].Diff));
  CHECK(!t::find(Steps[0].Diff, "module", "modified", "data layout"));
}

TEST(pipeline, inlined_function_removal_is_attributed) {
  auto L = parse("define internal i32 @sq(i32 %x) {\n  %r = mul i32 %x, %x\n"
                 "  ret i32 %r\n}\n"
                 "define i32 @f(i32 %a) {\n  %v = call i32 @sq(i32 %a)\n"
                 "  ret i32 %v\n}\n");
  PipelineConfig C;
  C.Passes = "cgscc(inline),globaldce";
  C.UseTarget = false;
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK_MSG(tracePasses(*L, C, DiffOptions(), "", Steps, R, Err), Err);
  bool Inlined = false, Removed = false;
  for (auto &S : Steps) {
    if (t::find(S.Diff, "instruction", "removed", "call i32 @sq"))
      Inlined = true;
    if (t::find(S.Diff, "function", "removed", "@sq"))
      Removed = true;
  }
  CHECK(Inlined);
  CHECK(Removed);
}

TEST(pipeline, foreign_blockaddress_uses_whole_module_snapshots) {
  auto L =
      parse("define void @g() {\nentry:\n  br label %bb\nbb:\n  ret void\n}\n"
            "define ptr @f(i32 %a) {\n  %x = add i32 %a, 0\n"
            "  ret ptr blockaddress(@g, %bb)\n}\n");
  PipelineConfig C;
  C.Passes = "function(instcombine)";
  C.UseTarget = false;
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK_MSG(tracePasses(*L, C, DiffOptions(), "", Steps, R, Err), Err);
  bool Note = false;
  for (auto &N : R.Notes)
    Note |= N.find("whole-module snapshots") != std::string::npos;
  CHECK(Note);
  CHECK(!Steps.empty());
}

TEST(pipeline, debug_info_mode_snapshots) {
  auto L = load("real/kernels.O0.ll");
  PipelineConfig C;
  C.Passes = "function(sroa)";
  DiffOptions O;
  O.CompareDebug = true;
  O.Functions = {"classify"};
  std::vector<PassStep> Steps;
  PipelineResult R;
  std::string Err;
  CHECK_MSG(tracePasses(*L, C, O, "", Steps, R, Err), Err);
  CHECK_EQ(Steps.size(), 1u);
}
