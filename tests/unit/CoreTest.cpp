#include "Pipeline.h"
#include "Test.h"

using namespace irdiff;

static Pairs L(std::vector<uint64_t> A, std::vector<uint64_t> B) {
  return lcs(A, B);
}

TEST(core, lcs_identical) {
  auto P = L({1, 2, 3}, {1, 2, 3});
  CHECK_EQ(P.size(), 3u);
  CHECK_EQ(P[2].first, 2u);
}

TEST(core, lcs_empty) {
  CHECK(L({}, {}).empty());
  CHECK(L({1, 2}, {}).empty());
  CHECK(L({}, {1}).empty());
}

TEST(core, lcs_insert_delete) {
  auto P = L({1, 2, 3, 4}, {1, 3, 4, 5});
  CHECK_EQ(P.size(), 3u);
  CHECK_EQ(P[0].first, 0u);
  CHECK_EQ(P[1].first, 2u);
  CHECK_EQ(P[1].second, 1u);
  CHECK_EQ(P[2].first, 3u);
  CHECK_EQ(P[2].second, 2u);
}

TEST(core, lcs_is_monotone_and_maximal) {
  std::vector<uint64_t> A = {7, 1, 9, 2, 9, 3, 4, 7, 5};
  std::vector<uint64_t> B = {1, 2, 7, 3, 9, 4, 5, 7};
  auto P = lcs(A, B);
  for (unsigned I = 1; I < P.size(); ++I) {
    CHECK(P[I].first > P[I - 1].first);
    CHECK(P[I].second > P[I - 1].second);
  }
  for (auto [X, Y] : P)
    CHECK_EQ(A[X], B[Y]);
  CHECK_EQ(P.size(), 5u);
}

TEST(core, lcs_cap_keeps_prefix_suffix) {
  std::vector<uint64_t> A = {1}, B = {1};
  for (unsigned I = 0; I < 50; ++I) {
    A.push_back(100 + I);
    B.push_back(200 + I);
  }
  A.push_back(9);
  B.push_back(9);
  auto P = lcs(A, B, 10);
  CHECK_EQ(P.size(), 2u);
}

TEST(core, similarity_bounds) {
  CHECK_EQ(similarity({}, {}), 1.0);
  CHECK_EQ(similarity({1, 2}, {3, 4}), 0.0);
  CHECK_EQ(similarity({1, 2}, {1, 2}), 1.0);
  CHECK_EQ(similarity({1, 2, 3, 4}, {1, 2}), 2.0 * 2 / 6);
}

TEST(core, strip_refs) {
  CHECK_EQ(stripRefs("  %x = load i32, ptr %p, align 4, !tbaa !5, !range !7"),
           std::string("%x = load i32, ptr %p, align 4"));
  CHECK_EQ(stripRefs("call void @f() #3, !dbg !12"),
           std::string("call void @f()"));
  CHECK_EQ(stripRefs("call void @llvm.foo(metadata !4)"),
           std::string("call void @llvm.foo(metadata !_)"));
  CHECK_EQ(stripRefs("@s = constant [6 x i8] c\"a, !b#1\\00\""),
           std::string("@s = constant [6 x i8] c\"a, !b#1\\00\""));
}

TEST(core, rewrite_tokens) {
  std::vector<std::string> Seen;
  std::string R =
      rewrite("%__v3 = add i32 %__v1, %struct.__v2 ; br label %__b4 @g @\"q "
              "r\" c\"%__v9\"",
              [&](Tok T, unsigned Id, llvm::StringRef Raw, std::string &Out) {
                Seen.push_back(std::string(T == Tok::Local   ? "L"
                                           : T == Tok::Block ? "B"
                                                             : "G") +
                               std::to_string(Id) + Raw.str());
                Out += "#";
              });
  CHECK_EQ(R, std::string(
                  "# = add i32 #, %struct.__v2 ; br label # # # c\"%__v9\""));
  CHECK_EQ(Seen.size(), 5u);
  CHECK_EQ(Seen[0], std::string("L3%__v3"));
  CHECK_EQ(Seen[2], std::string("B4%__b4"));
  CHECK_EQ(Seen[4], std::string("G0@\"q r\""));
}

TEST(core, globs) {
  CHECK(matchesAny({"foo*"}, "foobar"));
  CHECK(matchesAny({"foo*"}, "@foobar"));
  CHECK(!matchesAny({"foo"}, "foobar"));
  CHECK(matchesAny({"a", "b?"}, "bx"));
  CHECK(!matchesAny({}, "x"));
}

TEST(core, keep_change_filters) {
  Change C;
  C.Cat = "instruction";
  C.Kind = "added";
  C.Significance = Sig::Minor;
  C.Block = "loop";
  Options O;
  CHECK(keepChange(C, O));
  O.MinSig = Sig::Major;
  CHECK(!keepChange(C, O));
  O.MinSig = Sig::Cosmetic;
  O.Categories = {"cfg"};
  CHECK(!keepChange(C, O));
  O.Categories = {"instruction"};
  O.Kinds = {"removed"};
  CHECK(!keepChange(C, O));
  O.Kinds = {"added"};
  O.Blocks = {"lo*"};
  CHECK(keepChange(C, O));
  O.Blocks = {"entry"};
  CHECK(!keepChange(C, O));
}

TEST(core, flatten_pipeline) {
  auto F = flattenPipeline("verify,function(sroa<modify-cfg>,early-cse<>),"
                           "cgscc(inline<only-mandatory>),x");
  CHECK_EQ(F.size(), 5u);
  CHECK_EQ(F[0], std::string("verify"));
  CHECK_EQ(F[1], std::string("function/sroa<modify-cfg>"));
  CHECK_EQ(F[2], std::string("function/early-cse<>"));
  CHECK_EQ(F[3], std::string("cgscc/inline<only-mandatory>"));
  CHECK_EQ(F[4], std::string("x"));
}

TEST(core, flatten_pipeline_params_with_separators) {
  auto F = flattenPipeline(
      "function(simplifycfg<bonus-inst-threshold=1;no-switch-to-lookup>)");
  CHECK_EQ(F.size(), 1u);
  CHECK_EQ(
      F[0],
      std::string(
          "function/simplifycfg<bonus-inst-threshold=1;no-switch-to-lookup>"));
}

TEST(core, significance_names) {
  CHECK_EQ(std::string(sigName(Sig::Cosmetic)), std::string("cosmetic"));
  CHECK_EQ(std::string(sigName(Sig::Major)), std::string("major"));
}
