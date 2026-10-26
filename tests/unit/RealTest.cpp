#include "Test.h"

using namespace irdiff;

static ModuleDiff diffFiles(const std::string &A, const std::string &B,
                            DiffOptions O = {}) {
  LoadResult LA = loadModule(t::fixture(A), true);
  LoadResult LB = loadModule(t::fixture(B), true);
  if (!LA.L || !LB.L)
    t::fail(__FILE__, __LINE__, LA.Error + LB.Error);
  O.Rows = true;
  return diffModules(*LA.L, *LB.L, O);
}

TEST(real, self_diff_is_clean) {
  for (const char *F :
       {"real/kernels.O0.ll", "real/kernels.O1.ll", "real/kernels.O2.ll"}) {
    ModuleDiff D = diffFiles(F, F);
    CHECK_MSG(t::changes(D).empty(), std::string(F) + t::dump(D));
  }
}

TEST(real, text_and_bitcode_of_same_build_match) {
  ModuleDiff D = diffFiles("real/kernels.O2.ll", "real/kernels.O2.bc");
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
}

TEST(real, o0_to_o2_explains_major_transformations) {
  ModuleDiff D = diffFiles("real/kernels.O0.ll", "real/kernels.O2.ll");
  CHECK_MSG(t::find(D, "function", "removed", "@square"), t::dump(D));
  CHECK(t::find(D, "instruction", "removed", "alloca"));
  const Change *Attr =
      t::find(D, "attribute", "modified", "function-attributes");
  CHECK(Attr);
  bool Plus = false;
  for (auto &X : Attr->Details)
    Plus |= !X.empty() && X[0] == '+';
  CHECK(Plus);
  CHECK(t::find(D, "loop", "added"));
  CHECK(t::find(D, "intrinsic", "added"));
  unsigned Mod = 0;
  for (auto &F : D.Funcs)
    Mod += F.Status == "modified";
  CHECK_EQ(Mod, 4u);
}

TEST(real, o1_to_o2_vectorization_is_structural) {
  ModuleDiff D = diffFiles("real/kernels.O1.ll", "real/kernels.O2.ll");
  const Change *L = t::find(D, "loop", "modified", "%for.body");
  CHECK_MSG(L, t::dump(D));
  CHECK(t::hasDetail(L, "llvm.loop.isvectorized"));
  CHECK(t::find(D, "loop", "added", "vector.body"));
  CHECK(t::find(D, "block", "added", "vector.body"));
  const FuncDiff *Classify = nullptr;
  for (auto &F : D.Funcs)
    if (F.NameA == "@classify")
      Classify = &F;
  CHECK(Classify);
  CHECK_MSG(Classify->Status == "identical" || Classify->Status == "equivalent",
            Classify->Status);
}

TEST(real, matching_keeps_preheader_identity) {
  ModuleDiff D = diffFiles("real/kernels.O1.ll", "real/kernels.O2.ll");
  for (auto &F : D.Funcs)
    for (auto &C : F.Changes)
      if (C.Cat == "instruction")
        for (auto &X : C.Details)
          CHECK_MSG(X.find("same name, different matched entity") ==
                        std::string::npos,
                    F.NameA + ": " + C.Subject + " / " + X);
}

TEST(real, deterministic) {
  ModuleDiff A = diffFiles("real/kernels.O0.ll", "real/kernels.O2.ll");
  ModuleDiff B = diffFiles("real/kernels.O0.ll", "real/kernels.O2.ll");
  CHECK_EQ(t::dump(A), t::dump(B));
}
