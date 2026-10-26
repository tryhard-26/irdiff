#include "Test.h"

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace irdiff;

TEST(input, malformed_text_reports_location) {
  LoadResult R = loadModule(t::fixture("malformed.ll"), true);
  CHECK(!R.L);
  CHECK_EQ(R.Code, int(ExitInput));
  CHECK_MSG(R.Error.find("malformed.ll:4:") != std::string::npos, R.Error);
}

TEST(input, verifier_failure_is_invalid_ir) {
  LoadResult R = loadModule(t::fixture("invalid.ll"), true);
  CHECK(!R.L);
  CHECK_EQ(R.Code, int(ExitInput));
  CHECK_MSG(R.Error.find("invalid IR") != std::string::npos, R.Error);
  CHECK_MSG(R.Error.find("dominate") != std::string::npos, R.Error);
}

TEST(input, verifier_can_be_skipped) {
  LoadResult R = loadModule(t::fixture("invalid.ll"), false);
  CHECK(R.L != nullptr);
}

TEST(input, missing_file) {
  LoadResult R = loadModule(t::fixture("does-not-exist.ll"), true);
  CHECK_EQ(R.Code, int(ExitInput));
  CHECK(R.Error.find("cannot read") != std::string::npos);
}

TEST(input, empty_input) {
  LoadResult R = loadModuleFromBuffer("", "empty.ll", true);
  CHECK_EQ(R.Code, int(ExitInput));
  CHECK(R.Error.find("empty input") != std::string::npos);
}

TEST(input, non_ir_inputs_are_identified) {
  struct {
    std::string Data, Expect;
  } Cases[] = {
      {std::string("\x7f"
                   "ELF\x02\x01\x01",
                   7),
       "ELF"},
      {std::string("\xcf\xfa\xed\xfe\x07\x00", 6), "Mach-O"},
      {"!<arch>\nfoo", "archive"},
      {"--- |\n  define void @f() { ret void }\n...\n---\nname: f\nbody: |\n",
       "MIR"},
      {"\t.text\n\t.globl f\nf:\n\tret\n", "assembly"}};
  for (auto &C : Cases) {
    LoadResult R = loadModuleFromBuffer(C.Data, "x", true);
    CHECK_EQ(R.Code, int(ExitInput));
    CHECK_MSG(R.Error.find("unsupported input") != std::string::npos, R.Error);
    CHECK_MSG(R.Error.find(C.Expect) != std::string::npos, R.Error);
  }
}

TEST(input, bitcode_round_trip) {
  LoadResult T = loadModule(t::fixture("loops.ll"), true);
  CHECK(T.L);
  llvm::SmallVector<char, 0> Buf;
  llvm::raw_svector_ostream OS(Buf);
  llvm::WriteBitcodeToFile(*T.L->M, OS);
  LoadResult B = loadModuleFromBuffer(llvm::StringRef(Buf.data(), Buf.size()),
                                      "loops.bc", true);
  CHECK_MSG(B.L, B.Error);
  CHECK_EQ(B.L->Info.Format, std::string("bitcode"));
  CHECK_MSG(llvm::StringRef(B.L->Info.Producer).starts_with("LLVM"),
            B.L->Info.Producer);
  CHECK_EQ(B.L->Info.ProducerMajor, llvmMajor());
  ModuleDiff D = diffModules(*T.L, *B.L, DiffOptions());
  CHECK_MSG(t::changes(D).empty(), t::dump(D));
  bool FormatNote = false;
  for (auto &N : D.Compat)
    FormatNote |= N.find("different formats") != std::string::npos;
  CHECK(FormatNote);
}

TEST(input, corrupt_bitcode) {
  std::string Bad("BC\xC0\xDE\x35\x14\x00\x00\x05\x00\x00\x00\x62\x0c\x30\x24",
                  16);
  LoadResult R = loadModuleFromBuffer(Bad, "bad.bc", true);
  CHECK(!R.L);
  CHECK_EQ(R.Code, int(ExitInput));
  CHECK_MSG(R.Error.find("cannot read bitcode") != std::string::npos, R.Error);
}

TEST(input, newer_producer_hint_on_parse_failure) {
  std::string Text =
      "define void @f() {\n  frobnicate\n}\n!llvm.ident = !{!0}\n"
      "!0 = !{!\"clang version 99.0.0\"}\n";
  LoadResult R = loadModuleFromBuffer(Text, "new.ll", true);
  CHECK_EQ(R.Code, int(ExitIncompatible));
  CHECK_MSG(R.Error.find("produced by LLVM 99") != std::string::npos, R.Error);
}

TEST(input, older_producer_parse_failure_is_plain_error) {
  std::string Text =
      "define void @f() {\n  frobnicate\n}\n!llvm.ident = !{!0}\n"
      "!0 = !{!\"clang version 3.0\"}\n";
  LoadResult R = loadModuleFromBuffer(Text, "old.ll", true);
  CHECK_EQ(R.Code, int(ExitInput));
}

TEST(input, producer_versions) {
  CHECK_EQ(parseProducerMajor("LLVM19.1.7"), 19);
  CHECK_EQ(parseProducerMajor("clang version 22.1.4 (https://...)"), 22);
  CHECK_EQ(parseProducerMajor("Homebrew clang version 21.0.0"), 21);
  CHECK_EQ(parseProducerMajor("Apple clang version 17.0.0"), 0);
  CHECK_EQ(parseProducerMajor("rustc"), 0);
}

TEST(input, info_is_filled) {
  LoadResult R = loadModule(t::fixture("real/kernels.O2.ll"), true);
  CHECK(R.L);
  const InputInfo &I = R.L->Info;
  CHECK_EQ(I.Format, std::string("text"));
  CHECK_EQ(I.Triple, std::string("x86_64-unknown-linux-gnu"));
  CHECK(I.Functions >= 3);
  CHECK(I.Instructions > 50);
}

TEST(input, compat_notes) {
  ModuleDiff D;
  D.A.Path = "a";
  D.B.Path = "b";
  D.A.Triple = "x86_64-unknown-linux-gnu";
  D.B.Triple = "aarch64-unknown-linux-gnu";
  D.A.Format = D.B.Format = "text";
  D.B.Producer = "LLVM999.0";
  D.B.ProducerMajor = 999;
  checkCompat(D);
  CHECK(D.Incompatible);
  bool Newer = false, Triple = false;
  for (auto &N : D.Compat) {
    Newer |= N.find("newer LLVM") != std::string::npos;
    Triple |= N.find("target triples differ") != std::string::npos;
  }
  CHECK(Newer);
  CHECK(Triple);
}

TEST(input, bitcode_fixture_from_llvm19) {
  LoadResult R = loadModule(t::fixture("real/kernels.O2.bc"), true);
  CHECK_MSG(R.L, R.Error);
  CHECK_EQ(R.L->Info.Format, std::string("bitcode"));
  CHECK_EQ(R.L->Info.ProducerMajor, 19);
}
