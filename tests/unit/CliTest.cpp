#include "Test.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace irdiff;

static std::string F(const std::string &N) { return t::fixture(N); }

static llvm::json::Object parse(const std::string &S) {
  auto V = llvm::json::parse(S);
  if (!V)
    t::fail(__FILE__, __LINE__,
            "invalid JSON: " + llvm::toString(V.takeError()) + "\n" + S);
  if (!V->getAsObject())
    t::fail(__FILE__, __LINE__, "JSON is not an object");
  return *V->getAsObject();
}

TEST(cli, help_and_version) {
  t::Run H = t::run({"--help"});
  CHECK_EQ(H.Exit, 0);
  CHECK(H.Out.find("Exit status") != std::string::npos);
  CHECK(H.Out.find("pipelines") != std::string::npos);
  t::Run V = t::run({"--version"});
  CHECK_EQ(V.Exit, 0);
  CHECK(V.Out.find("irdiff") != std::string::npos);
  CHECK(V.Out.find("LLVM " + llvmVersion()) != std::string::npos);
  t::Run S = t::run({"diff", "--help"});
  CHECK_EQ(S.Exit, 0);
  CHECK(S.Out.find("--min-significance") != std::string::npos);
}

TEST(cli, exit_codes) {
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_a.ll")}).Exit, 0);
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_b.ll")}).Exit, 1);
  CHECK_EQ(t::run({F("rename_a.ll"), F("rename_b.ll")}).Exit, 0);
  CHECK_EQ(t::run({F("rename_a.ll"), F("rename_b.ll"), "--all"}).Exit, 1);
  CHECK_EQ(t::run({"diff", F("flags_a.ll")}).Exit, 2);
  CHECK_EQ(t::run({"--bogus-flag"}).Exit, 2);
  CHECK_EQ(t::run({F("flags_a.ll"), F("malformed.ll")}).Exit, 3);
  CHECK_EQ(t::run({F("flags_a.ll"), F("invalid.ll")}).Exit, 3);
  CHECK_EQ(t::run({F("flags_a.ll"), F("missing.ll")}).Exit, 3);
  CHECK_EQ(t::run({F("flags_a.ll"), F("arm.ll"), "--require-compatible"}).Exit,
           4);
  CHECK_EQ(t::run({"passes", F("loops.ll"), "-p", "bogus"}).Exit, 2);
  CHECK_EQ(t::run({}).Exit, 2);
}

TEST(cli, error_messages_are_prefixed) {
  t::Run R = t::run({F("flags_a.ll"), F("malformed.ll")});
  CHECK(R.Out.empty());
  CHECK_MSG(R.Err.find("irdiff: error:") != std::string::npos, R.Err);
  CHECK(R.Err.find("malformed.ll:4:") != std::string::npos);
}

TEST(cli, fail_on_threshold) {
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_b.ll"), "--fail-on=never"}).Exit,
           0);
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_b.ll"), "--fail-on=major"}).Exit,
           1);
  CHECK_EQ(
      t::run({F("rename_a.ll"), F("rename_b.ll"), "--all", "--fail-on=minor"})
          .Exit,
      0);
  CHECK_EQ(
      t::run({F("flags_a.ll"), F("flags_b.ll"), "--fail-on=sometimes"}).Exit,
      2);
}

TEST(cli, text_report) {
  t::Run R = t::run({F("flags_a.ll"), F("flags_b.ll")});
  CHECK(R.Out.find("--- A:") != std::string::npos);
  CHECK(R.Out.find("@inc  modified") != std::string::npos);
  CHECK(R.Out.find("flags: +nsw") != std::string::npos);
  CHECK(R.Out.find("summary:") != std::string::npos);
  CHECK(R.Out.find("\x1b[") == std::string::npos);
  t::Run C = t::run({F("flags_a.ll"), F("flags_b.ll"), "--color=always"});
  CHECK(C.Out.find("\x1b[") != std::string::npos);
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_b.ll"), "--color=sometimes"}).Exit,
           2);
}

TEST(cli, json_report_schema) {
  t::Run R = t::run({F("flags_a.ll"), F("flags_b.ll"), "--format=json"});
  CHECK_EQ(R.Exit, 1);
  auto O = parse(R.Out);
  CHECK(O.getObject("tool"));
  CHECK_EQ(O.getObject("tool")->getString("name")->str(),
           std::string("irdiff"));
  CHECK(O.getObject("inputs")->getObject("a"));
  auto *S = O.getObject("summary");
  CHECK(S);
  CHECK_EQ(*S->getInteger("modified"), 1);
  CHECK_EQ(*S->getInteger("identical"), 1);
  CHECK_EQ(S->getString("max_significance")->str(), std::string("major"));
  auto *Fs = O.getArray("functions");
  CHECK(Fs);
  bool Found = false;
  for (auto &V : *Fs) {
    auto *Fn = V.getAsObject();
    if (Fn->getString("name_a")->str() != "@inc")
      continue;
    for (auto &Ch : *Fn->getArray("changes"))
      Found |= Ch.getAsObject()->getString("kind")->str() == "flags-changed";
  }
  CHECK(Found);
  CHECK(O.getArray("module_changes"));
  CHECK(O.getArray("hints"));
}

TEST(cli, json_is_deterministic) {
  std::vector<std::string> Args = {
      F("real/kernels.O0.ll"), F("real/kernels.O2.ll"), "--format=json", "-v"};
  t::Run A = t::run(Args), B = t::run(Args);
  CHECK_EQ(A.Exit, 1);
  CHECK(A.Out == B.Out);
  auto O = parse(A.Out);
  bool Rows = false;
  for (auto &V : *O.getArray("functions"))
    Rows |= V.getAsObject()->getArray("rows") != nullptr;
  CHECK(Rows);
}

TEST(cli, unified_and_side_by_side) {
  t::Run U = t::run({F("flags_a.ll"), F("flags_b.ll"), "--format=unified"});
  CHECK(U.Out.find("--- a/") != std::string::npos);
  CHECK(U.Out.find("@@ define @inc @@") != std::string::npos);
  CHECK(U.Out.find("-  %r = add i32 %x, 1") != std::string::npos);
  CHECK(U.Out.find("+  %r = add nsw i32 %x, 1") != std::string::npos);
  CHECK(U.Out.find("@same") == std::string::npos);
  t::Run Z =
      t::run({F("flags_a.ll"), F("flags_b.ll"), "-f", "unified", "-U", "0"});
  CHECK(Z.Out.find(" entry:") == std::string::npos);
  t::Run S = t::run({F("flags_a.ll"), F("flags_b.ll"), "--format=side-by-side",
                     "--width=100"});
  CHECK(S.Out.find("=== @inc ===") != std::string::npos);
  CHECK(S.Out.find(" | ") != std::string::npos);
  t::Run C = t::run({F("rename_a.ll"), F("rename_b.ll"), "--format=unified",
                     "--names=canonical", "--all"});
  CHECK_EQ(C.Exit, 1);
}

TEST(cli, filters) {
  std::vector<std::string> Base = {F("real/kernels.O0.ll"),
                                   F("real/kernels.O2.ll"), "--format=json"};
  auto With = [&](std::vector<std::string> Extra) {
    auto A = Base;
    A.insert(A.end(), Extra.begin(), Extra.end());
    return parse(t::run(A).Out);
  };
  auto O = With({"--function=classify"});
  for (auto &V : *O.getArray("functions"))
    CHECK_EQ(V.getAsObject()->getString("name_a")->str(),
             std::string("@classify"));
  auto C = With({"--category=cfg"});
  for (auto &V : *C.getArray("functions"))
    for (auto &Ch : *V.getAsObject()->getArray("changes"))
      CHECK_EQ(Ch.getAsObject()->getString("category")->str(),
               std::string("cfg"));
  auto K = With({"--change=removed,added"});
  for (auto &V : *K.getArray("functions"))
    for (auto &Ch : *V.getAsObject()->getArray("changes")) {
      std::string Kind = Ch.getAsObject()->getString("kind")->str();
      CHECK(Kind == "removed" || Kind == "added");
    }
  auto M = With({"--major"});
  CHECK_EQ(*M.getObject("summary")
                ->getObject("by_significance")
                ->getInteger("minor"),
           0);
  CHECK_EQ(
      t::run({F("flags_a.ll"), F("flags_b.ll"), "--category=nonsense"}).Exit,
      2);
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_b.ll"), "--change=bogus"}).Exit,
           2);
  CHECK_EQ(t::run({F("flags_a.ll"), F("flags_b.ll"), "--function=same"}).Exit,
           0);
}

TEST(cli, quiet_output_file_and_stdin) {
  t::Run Q = t::run({F("flags_a.ll"), F("flags_b.ll"), "-q"});
  CHECK_EQ(Q.Exit, 1);
  CHECK(Q.Out.empty());
  std::string Out = t::scratch("report.txt");
  t::Run O = t::run({F("flags_a.ll"), F("flags_b.ll"), "-o", Out});
  CHECK_EQ(O.Exit, 1);
  CHECK(O.Out.empty());
  auto Buf = llvm::MemoryBuffer::getFile(Out);
  CHECK(Buf && (*Buf)->getBuffer().contains("flags: +nsw"));
  auto In = llvm::MemoryBuffer::getFile(F("flags_b.ll"));
  t::Run S = t::run({F("flags_a.ll"), "-"}, (*In)->getBuffer().str());
  CHECK_EQ(S.Exit, 1);
  CHECK(S.Out.find("<stdin>") != std::string::npos);
  CHECK_EQ(t::run({"-", "-"}).Exit, 2);
}

TEST(cli, list_categories) {
  t::Run R = t::run({"--list-categories"});
  CHECK_EQ(R.Exit, 0);
  CHECK(R.Out.find("categories:") != std::string::npos);
  CHECK(R.Out.find("mssa") != std::string::npos);
}

TEST(cli, compatibility_is_reported) {
  t::Run R = t::run({F("flags_a.ll"), F("arm.ll")});
  CHECK(R.Out.find("target triples differ") != std::string::npos);
  auto O = parse(t::run({F("flags_a.ll"), F("arm.ll"), "--format=json"}).Out);
  CHECK_EQ(*O.getObject("compatibility")->getBoolean("incompatible"), true);
}

TEST(cli, passes_command) {
  t::Run R = t::run(
      {"passes", F("real/kernels.O0.ll"), "-p", "function(sroa,instcombine)"});
  CHECK_EQ(R.Exit, 1);
  CHECK(R.Out.find("SROAPass (sroa)") != std::string::npos);
  t::Run J = t::run({"passes", F("real/kernels.O0.ll"), "-O1", "--format=json",
                     "--function=classify"});
  auto O = parse(J.Out);
  CHECK(O.getArray("steps")->size() > 0);
  CHECK(O.getArray("executions")->size() > O.getArray("steps")->size());
  for (auto &S : *O.getArray("steps")) {
    auto *Fns = S.getAsObject()->getArray("changed_functions");
    if (Fns->size())
      CHECK_EQ((*Fns)[0].getAsString()->str(), std::string("classify"));
  }
  t::Run P = t::run({"passes", F("real/kernels.O0.ll"), "-O2", "--pass=licm",
                     "--function=scale_restrict"});
  CHECK(P.Out.find("LICMPass") != std::string::npos);
  CHECK(P.Out.find("now outside") != std::string::npos);
  CHECK(P.Out.find("SROAPass") == std::string::npos);
  t::Run L = t::run({"passes", "--from-log", F("real/kernels.passes.log")});
  CHECK_EQ(L.Exit, 1);
  CHECK(L.Out.find("InstCombinePass") != std::string::npos);
  t::Run U = t::run({"passes", F("loops.ll"), "-p", "function(instcombine)",
                     "--format=unified"});
  CHECK(U.Exit == 0 || U.Out.find("@@") != std::string::npos);
}

TEST(cli, pipelines_command) {
  t::Run R = t::run({"pipelines", F("real/kernels.O0.ll"), "-a", "default<O1>",
                     "-b", "default<O2>", "--hints"});
  CHECK_EQ(R.Exit, 1);
  CHECK(R.Out.find("pass pipeline") != std::string::npos);
  CHECK(R.Out.find("optimization remarks") != std::string::npos);
  CHECK(R.Out.find("[default<O1>]") != std::string::npos);
  t::Run S = t::run({"pipelines", F("real/kernels.O0.ll"), "-a", "default<O2>",
                     "-b", "default<O2>"});
  CHECK_MSG(S.Exit == 0, S.Out);
}

TEST(cli, analyze_and_investigate) {
  t::Run A = t::run({"analyze", F("loops.ll")});
  CHECK_EQ(A.Exit, 0);
  CHECK(A.Out.find("trip count: 100") != std::string::npos);
  auto J = parse(
      t::run({"analyze", F("loops.ll"), "--format=json", "--analysis=aa"}).Out);
  CHECK_EQ(J.getArray("functions")->size(), 2u);
  t::Run I = t::run({"investigate", F("alias_a.ll"), F("alias_b.ll")});
  CHECK_EQ(I.Exit, 1);
  CHECK(I.Out.find("potentially relevant differences") != std::string::npos);
  CHECK(I.Out.find("does not establish why") != std::string::npos);
  CHECK(I.Out.find("NoAlias -> MayAlias") != std::string::npos);
  t::Run W =
      t::run({"investigate", F("real/kernels.O1.ll"), F("real/kernels.O2.ll"),
              "--remarks-a", F("real/kernels.O1.opt.yaml"), "--remarks-b",
              F("real/kernels.O2.opt.yaml"), "--function=scale"});
  CHECK(W.Out.find("as reported by LLVM") != std::string::npos);
  CHECK_EQ(t::run({"diff", F("alias_a.ll"), F("alias_b.ll"), "--remarks-a",
                   F("nope.yaml")})
               .Exit,
           3);
}

TEST(cli, shorthand_and_bitcode_inputs) {
  t::Run R = t::run({F("real/kernels.O2.ll"), F("real/kernels.O2.bc")});
  CHECK_MSG(R.Exit == 0, R.Out + R.Err);
  CHECK(R.Out.find("different formats") != std::string::npos);
}
