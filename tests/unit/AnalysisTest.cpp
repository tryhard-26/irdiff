#include "Analyses.h"
#include "Test.h"

using namespace irdiff;

static ModuleDiff diffFiles(const std::string &A, const std::string &B,
                            DiffOptions O) {
  LoadResult LA = loadModule(t::fixture(A), true);
  LoadResult LB = loadModule(t::fixture(B), true);
  if (!LA.L || !LB.L)
    t::fail(__FILE__, __LINE__, LA.Error + LB.Error);
  return diffModules(*LA.L, *LB.L, O);
}

TEST(analysis, alias_precision_change) {
  DiffOptions O;
  O.RunAnalyses = true;
  ModuleDiff D = diffFiles("alias_a.ll", "alias_b.ll", O);
  const Change *C = t::find(D, "analysis", "modified", "alias:");
  CHECK_MSG(C, t::dump(D));
  CHECK_EQ(C->Before, std::string("NoAlias"));
  CHECK_EQ(C->After, std::string("MayAlias"));
  CHECK(t::find(D, "attribute", "modified", "parameter-attributes"));
}

TEST(analysis, memory_ssa_clobber_change) {
  DiffOptions O;
  O.RunAnalyses = true;
  O.Analyses = {"mssa"};
  ModuleDiff D = diffFiles("alias_a.ll", "alias_b.ll", O);
  const Change *C = t::find(D, "analysis", "modified", "clobbering access");
  CHECK_MSG(C, t::dump(D));
  CHECK_EQ(C->Before, std::string("liveOnEntry"));
  CHECK(C->After.find("store") != std::string::npos);
  CHECK(!t::find(D, "analysis", "modified", "alias:"));
}

TEST(analysis, analyses_off_by_default) {
  ModuleDiff D = diffFiles("alias_a.ll", "alias_b.ll", DiffOptions());
  CHECK_EQ(t::count(D, "analysis"), 0u);
}

TEST(analysis, dominator_and_scev_changes) {
  std::string A =
      "define i32 @f(i32 %n) {\nentry:\n  br label %loop\nloop:\n"
      "  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]\n  %inc = add nuw nsw i32 "
      "%i, 1\n"
      "  %c = icmp ult i32 %inc, 100\n  br i1 %c, label %loop, label %exit\n"
      "exit:\n  ret i32 %inc\n}\n";
  std::string B =
      "define i32 @f(i32 %n) {\nentry:\n  br label %pre\npre:\n  br label "
      "%loop\nloop:\n"
      "  %i = phi i32 [ 0, %pre ], [ %inc, %loop ]\n  %inc = add nuw nsw i32 "
      "%i, 1\n"
      "  %c = icmp ult i32 %inc, %n\n  br i1 %c, label %loop, label %exit\n"
      "exit:\n  ret i32 %inc\n}\n";
  DiffOptions O;
  O.RunAnalyses = true;
  ModuleDiff D = t::diff(A, B, O);
  CHECK_MSG(t::find(D, "analysis", "modified", "immediate dominator of %loop"),
            t::dump(D));
  const Change *S = t::find(D, "analysis", "modified", "scalar evolution");
  CHECK_MSG(S, t::dump(D));
  CHECK(t::hasDetail(S, "constant trip count: 100 -> unknown"));
}

TEST(analysis, analyze_module_facts) {
  LoadResult L = loadModule(t::fixture("loops.ll"), true);
  CHECK(L.L);
  auto Fs = analyzeModule(*L.L, Options());
  CHECK_EQ(Fs.size(), 2u);
  const Change *Loop = nullptr, *AA = nullptr, *MSSA = nullptr, *Dom = nullptr;
  for (auto &C : Fs[0].Changes)
    if (C.Subject.rfind("loop %", 0) == 0)
      Loop = &C;
  for (auto &C : Fs[1].Changes) {
    if (C.Subject == "alias analysis")
      AA = &C;
    if (C.Subject == "memory SSA clobbers")
      MSSA = &C;
    if (C.Subject == "dominator tree")
      Dom = &C;
  }
  CHECK(Loop);
  CHECK(t::hasDetail(Loop, "trip count: 100"));
  CHECK(AA);
  CHECK_MSG(AA->After.find("MayAlias 1") != std::string::npos, AA->After);
  CHECK(MSSA);
  CHECK(Dom);
  CHECK(t::hasDetail(Dom, "%body: idom %entry"));
}

TEST(analysis, analysis_filter) {
  LoadResult L = loadModule(t::fixture("loops.ll"), true);
  Options O;
  O.Analyses = {"dom"};
  O.Functions = {"copy"};
  auto Fs = analyzeModule(*L.L, O);
  CHECK_EQ(Fs.size(), 1u);
  CHECK_EQ(Fs[0].Changes.size(), 1u);
  CHECK_EQ(Fs[0].Changes[0].Subject, std::string("dominator tree"));
}

TEST(analysis, load_yaml_remarks) {
  std::vector<Remark> R;
  std::string Err;
  CHECK_MSG(loadRemarks(t::fixture("real/kernels.O2.opt.yaml"), R, Err), Err);
  CHECK(R.size() > 5);
  bool Vec = false;
  for (auto &X : R)
    Vec |= X.Pass == "loop-vectorize" && X.Name == "Vectorized" &&
           X.Type == "Passed";
  CHECK(Vec);
}

TEST(analysis, malformed_remarks_file) {
  std::vector<Remark> R;
  std::string Err;
  CHECK(!loadRemarks(t::fixture("malformed.ll"), R, Err));
  CHECK(!Err.empty());
  CHECK(!loadRemarks(t::fixture("nope.yaml"), R, Err));
}

TEST(analysis, remark_diff_and_hints) {
  std::vector<Remark> A, B;
  std::string Err;
  CHECK(loadRemarks(t::fixture("real/kernels.O1.opt.yaml"), A, Err));
  CHECK(loadRemarks(t::fixture("real/kernels.O2.opt.yaml"), B, Err));
  ModuleDiff D;
  diffRemarks(A, B, D, Options());
  const Change *V = t::find(D, "remark", "added", "loop-vectorize/Vectorized");
  CHECK_MSG(V, t::dump(D));
  CHECK(V->After.find("vectorized loop") != std::string::npos);
  CHECK(t::find(D, "remark", "removed", "[Missed] loop-vectorize"));
  addHints(D);
  const Change *H = t::find(D, "hint", "observation", "missed optimization");
  CHECK_MSG(H, t::dump(D));
  CHECK(H->Inferred);
  CHECK(t::hasDetail(H, "evidence:"));
}

TEST(analysis, remark_filter_by_function) {
  std::vector<Remark> A, B;
  std::string Err;
  CHECK(loadRemarks(t::fixture("real/kernels.O1.opt.yaml"), A, Err));
  CHECK(loadRemarks(t::fixture("real/kernels.O2.opt.yaml"), B, Err));
  ModuleDiff D;
  Options O;
  O.Functions = {"classify"};
  diffRemarks(A, B, D, O);
  for (auto &C : D.RemarkChanges)
    CHECK_EQ(C.Func, std::string("classify"));
}

TEST(analysis, hints_from_ir_evidence) {
  DiffOptions O;
  O.RunAnalyses = true;
  ModuleDiff D = diffFiles("alias_a.ll", "alias_b.ll", O);
  addHints(D);
  CHECK_MSG(t::find(D, "hint", "", "less precise"), t::dump(D));
  CHECK(t::find(D, "hint", "", "attributes were lost"));
  ModuleDiff F = diffFiles("flags_b.ll", "flags_a.ll", DiffOptions());
  addHints(F);
  CHECK_MSG(t::find(F, "hint", "", "Poison-generating flags"), t::dump(F));
}

TEST(analysis, hints_ignore_restrictive_attrs_and_cap_evidence) {
  auto Fn = [](const char *Attrs) {
    return std::string("define void @f() ") + Attrs + " {\n  ret void\n}\n";
  };
  ModuleDiff D = t::diff(Fn("noinline optnone"), Fn(""));
  addHints(D);
  CHECK_MSG(!t::find(D, "hint", "", "attributes were lost"), t::dump(D));
  D = t::diff(Fn("nounwind"), Fn(""));
  addHints(D);
  CHECK_MSG(t::find(D, "hint", "", "attributes were lost"), t::dump(D));

  Change Big;
  Big.Cat = "metadata";
  Big.Kind = "metadata-changed";
  Big.Func = "f";
  Big.Subject = "load";
  Big.Details.push_back("metadata: !tbaa " + std::string(5000, 'x'));
  ModuleDiff M;
  M.ModuleChanges.push_back(Big);
  addHints(M);
  const Change *H = t::find(M, "hint", "", "TBAA");
  CHECK(H);
  for (auto &Dt : H->Details)
    CHECK_MSG(Dt.size() <= 260, Dt.substr(0, 80));
}
