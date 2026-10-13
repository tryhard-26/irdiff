#include "Test.h"

using namespace irdiff;

static std::string
fn(const std::string &Body,
   const std::string &Sig = "define i32 @f(i32 %a, i32 %b)") {
  return Sig + " {\n" + Body + "\n}\n";
}

TEST(diff, identical_modules_have_no_changes) {
  std::string M = fn("entry:\n  %x = add i32 %a, %b\n  ret i32 %x");
  ModuleDiff D = t::diff(M, M);
  CHECK_MSG(t::changes(D).empty(), t::dump(D));
  CHECK_EQ(D.Funcs.size(), 1u);
  CHECK_EQ(D.Funcs[0].Status, std::string("identical"));
}

TEST(diff, value_and_label_names_are_ignored) {
  std::string A = fn(
      "entry:\n  %x = add i32 %a, %b\n  br label %next\nnext:\n  ret i32 %x");
  std::string B =
      fn("start:\n  %sum = add i32 %p, %q\n  br label %0\n0:\n  ret i32 %sum",
         "define i32 @f(i32 %p, i32 %q)");
  ModuleDiff D = t::diff(A, B);
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
  CHECK_EQ(D.Funcs[0].Status, std::string("equivalent"));
  CHECK(t::find(D, "block", "renamed"));
}

TEST(diff, rename_fixture_is_equivalent) {
  ModuleDiff D;
  LoadResult A = loadModule(t::fixture("rename_a.ll"), true);
  LoadResult B = loadModule(t::fixture("rename_b.ll"), true);
  CHECK(A.L && B.L);
  D = diffModules(*A.L, *B.L, DiffOptions());
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
  CHECK(t::find(D, "block", "moved"));
  CHECK(t::find(D, "instruction", "commuted", "phi incoming order"));
  CHECK(t::find(D, "instruction", "commuted", "commutative"));
}

TEST(diff, commuted_operands_are_equivalent) {
  ModuleDiff D = t::diff(fn("entry:\n  %x = mul i32 %a, %b\n  ret i32 %x"),
                         fn("entry:\n  %x = mul i32 %b, %a\n  ret i32 %x"));
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
  CHECK(t::find(D, "instruction", "commuted"));
}

TEST(diff, swapped_noncommutative_operands_are_a_change) {
  ModuleDiff D = t::diff(fn("entry:\n  %x = sub i32 %a, %b\n  ret i32 %x"),
                         fn("entry:\n  %x = sub i32 %b, %a\n  ret i32 %x"));
  const Change *C = t::find(D, "instruction", "operands-changed");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "operand 0"));
  CHECK(t::hasDetail(C, "operand 1"));
}

TEST(diff, commuted_with_different_flags_is_a_change) {
  ModuleDiff D = t::diff(fn("entry:\n  %x = add nsw i32 %a, %b\n  ret i32 %x"),
                         fn("entry:\n  %x = add i32 %b, %a\n  ret i32 %x"));
  CHECK_MSG(t::atLeast(D, Sig::Major) > 0, t::dump(D));
  CHECK(!t::find(D, "instruction", "commuted"));
}

TEST(diff, mirrored_compare_is_equivalent) {
  ModuleDiff D = t::diff(fn("entry:\n  %c = icmp sgt i32 %a, %b\n  %r = zext "
                            "i1 %c to i32\n  ret i32 %r"),
                         fn("entry:\n  %c = icmp slt i32 %b, %a\n  %r = zext "
                            "i1 %c to i32\n  ret i32 %r"));
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
  CHECK(t::find(D, "instruction", "commuted", "mirrored predicate"));
}

TEST(diff, switch_case_order_is_equivalent) {
  std::string Tail = "one:\n  ret i32 1\ntwo:\n  ret i32 2\ndef:\n  ret i32 0";
  ModuleDiff D = t::diff(fn("entry:\n  switch i32 %a, label %def [ i32 1, "
                            "label %one\n i32 2, label %two ]\n" +
                            Tail),
                         fn("entry:\n  switch i32 %a, label %def [ i32 2, "
                            "label %two\n i32 1, label %one ]\n" +
                            Tail));
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
}

TEST(diff, flag_change) {
  ModuleDiff D =
      t::diff(fn("entry:\n  %x = add i32 %a, %b\n  ret i32 %x"),
              fn("entry:\n  %x = add nuw nsw i32 %a, %b\n  ret i32 %x"));
  const Change *C = t::find(D, "instruction", "flags-changed");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "+nsw"));
  CHECK(t::hasDetail(C, "+nuw"));
  CHECK_EQ(int(C->Significance), int(Sig::Major));
}

TEST(diff, opcode_change) {
  ModuleDiff D = t::diff(
      fn("entry:\n  %x = add i32 %a, %b\n  %y = mul i32 %x, 3\n  ret i32 %y"),
      fn("entry:\n  %x = sub i32 %a, %b\n  %y = mul i32 %x, 3\n  ret i32 %y"));
  const Change *C = t::find(D, "instruction", "opcode-changed");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "add -> sub"));
  CHECK_EQ(D.Funcs[0].Stats.Modified, 1u);
}

TEST(diff, cast_opcode_change_via_def_use) {
  std::string S = "define i64 @f(i32 %a)";
  ModuleDiff D = t::diff(fn("entry:\n  %e = sext i32 %a to i64\n  %m = mul i64 "
                            "%e, 4\n  ret i64 %m",
                            S),
                         fn("entry:\n  %e = zext nneg i32 %a to i64\n  %m = "
                            "mul i64 %e, 4\n  ret i64 %m",
                            S));
  const Change *C = t::find(D, "instruction", "opcode-changed", "sext -> zext");
  CHECK_MSG(C, t::dump(D));
}

TEST(diff, predicate_and_alignment_changes) {
  std::string S = "define i1 @f(ptr %p, i32 %b)";
  ModuleDiff D = t::diff(fn("entry:\n  %v = load i32, ptr %p, align 4\n  %c = "
                            "icmp sgt i32 %v, %b\n  ret i1 %c",
                            S),
                         fn("entry:\n  %v = load i32, ptr %p, align 8\n  %c = "
                            "icmp sge i32 %v, %b\n  ret i1 %c",
                            S));
  CHECK_MSG(t::find(D, "instruction", "", "predicate: sgt -> sge"), t::dump(D));
  CHECK_MSG(t::find(D, "instruction", "", "align: 4 -> 8"), t::dump(D));
}

TEST(diff, type_change) {
  ModuleDiff D = t::diff(fn("entry:\n  %e = zext i32 %a to i64\n  ret i64 %e",
                            "define i64 @f(i32 %a)"),
                         fn("entry:\n  %e = zext i32 %a to i128\n  %t = trunc "
                            "i128 %e to i64\n  ret i64 %t",
                            "define i64 @f(i32 %a)"));
  CHECK_MSG(t::find(D, "instruction", "", "type: i64 -> i128"), t::dump(D));
}

TEST(diff, metadata_changes_and_ignore) {
  std::string S = "define i32 @f(ptr %p)";
  std::string A =
      fn("entry:\n  %v = load i32, ptr %p, !range !0\n  ret i32 %v", S) +
      "!0 = !{i32 0, i32 10}\n";
  std::string B =
      fn("entry:\n  %v = load i32, ptr %p, !range !0\n  ret i32 %v", S) +
      "!0 = !{i32 0, i32 20}\n";
  ModuleDiff D = t::diff(A, B);
  const Change *C = t::find(D, "metadata", "metadata-changed");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "i32 20"));
  DiffOptions O;
  O.CompareMetadata = false;
  ModuleDiff E = t::diff(A, B, O);
  CHECK_MSG(t::changes(E).empty(), t::dump(E));
}

TEST(diff, metadata_ids_are_normalized) {
  std::string S = "define i32 @f(ptr %p)";
  std::string A =
      fn("entry:\n  %v = load i32, ptr %p, !range !3\n  ret i32 %v", S) +
      "!3 = !{i32 0, i32 10}\n";
  std::string B =
      fn("entry:\n  %v = load i32, ptr %p, !range !0\n  ret i32 %v", S) +
      "!0 = !{i32 0, i32 10}\n";
  ModuleDiff D = t::diff(A, B);
  CHECK_MSG(t::changes(D).empty(), t::dump(D));
}

static std::string withDbg(unsigned Line) {
  return "define i32 @f(i32 %a) !dbg !4 {\nentry:\n  %x = add i32 %a, 1, !dbg "
         "!8\n"
         "  ret i32 %x, !dbg !8\n}\n"
         "!llvm.dbg.cu = !{!0}\n!llvm.module.flags = !{!2, !3}\n"
         "!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, "
         "emissionKind: FullDebug)\n!1 = !DIFile(filename: \"t.c\", directory: "
         "\"/\")\n"
         "!2 = !{i32 2, !\"Debug Info Version\", i32 3}\n!3 = !{i32 7, "
         "!\"Dwarf Version\", i32 5}\n"
         "!4 = distinct !DISubprogram(name: \"f\", scope: !1, file: !1, line: "
         "1, "
         "type: !5, unit: !0, spFlags: DISPFlagDefinition)\n!5 = "
         "!DISubroutineType(types: !6)\n"
         "!6 = !{null}\n!7 = !{}\n!8 = !DILocation(line: " +
         std::to_string(Line) + ", column: 3, scope: !4)\n";
}

TEST(diff, debug_info_ignored_by_default) {
  ModuleDiff D = t::diff(withDbg(2), withDbg(5));
  CHECK_MSG(t::changes(D).empty(), t::dump(D));
  DiffOptions O;
  O.CompareDebug = true;
  ModuleDiff E = t::diff(withDbg(2), withDbg(5), O);
  CHECK_MSG(t::find(E, "debug", "modified", "2:3 -> 5:3"), t::dump(E));
}

TEST(diff, added_and_removed_instructions) {
  std::string Decl = "declare void @g()\ndeclare void @h()\n";
  ModuleDiff D = t::diff(
      Decl + fn("entry:\n  call void @g()\n  ret i32 %a"),
      Decl + fn("entry:\n  call void @h()\n  call void @h()\n  ret i32 %a"));
  CHECK_EQ(t::count(D, "instruction", "added") +
               t::count(D, "instruction", "modified") +
               t::count(D, "instruction", "operands-changed"),
           2u);
  CHECK_EQ(D.Funcs[0].Stats.InstsA, 2u);
  CHECK_EQ(D.Funcs[0].Stats.InstsB, 3u);
}

static const char *LoopA = R"(
define void @f(ptr %dst, ptr noalias %k, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %kv = load i32, ptr %k, align 4
  %g = getelementptr i32, ptr %dst, i32 %i
  store i32 %kv, ptr %g, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
)";

static const char *LoopB = R"(
define void @f(ptr %dst, ptr noalias %k, i32 %n) {
entry:
  %kv = load i32, ptr %k, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %g = getelementptr i32, ptr %dst, i32 %i
  store i32 %kv, ptr %g, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
)";

TEST(diff, hoisted_instruction_is_moved_out_of_loop) {
  ModuleDiff D = t::diff(LoopA, LoopB);
  const Change *C = t::find(D, "instruction", "moved", "load i32");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "loop depth 1"));
  CHECK(t::hasDetail(C, "now outside 1 enclosing loop"));
  CHECK_EQ(t::count(D, "instruction", "added"), 0u);
  CHECK_EQ(t::count(D, "instruction", "removed"), 0u);
}

TEST(diff, sunk_instruction_reports_new_loop_nesting) {
  ModuleDiff D = t::diff(LoopB, LoopA);
  const Change *C = t::find(D, "instruction", "moved", "load i32");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "now inside 1 additional loop"));
}

TEST(diff, reorder_significance_depends_on_side_effects) {
  ModuleDiff P = t::diff(fn("entry:\n  %x = add i32 %a, 1\n  %y = add i32 %b, "
                            "2\n  %z = add i32 %x, %y\n  ret i32 %z"),
                         fn("entry:\n  %y = add i32 %b, 2\n  %x = add i32 %a, "
                            "1\n  %z = add i32 %x, %y\n  ret i32 %z"));
  const Change *C = t::find(P, "instruction", "reordered");
  CHECK_MSG(C, t::dump(P));
  CHECK_EQ(int(C->Significance), int(Sig::Cosmetic));
  std::string S = "define void @f(ptr %p, ptr %q)";
  ModuleDiff M = t::diff(
      fn("entry:\n  store i32 1, ptr %p\n  store i32 2, ptr %q\n  ret void", S),
      fn("entry:\n  store i32 2, ptr %q\n  store i32 1, ptr %p\n  ret void",
         S));
  const Change *R = t::find(M, "instruction", "reordered");
  CHECK_MSG(R, t::dump(M));
  CHECK_EQ(int(R->Significance), int(Sig::Major));
}

TEST(diff, merged_block) {
  std::string S = "define i32 @f(ptr %p, i32 %a)";
  ModuleDiff D =
      t::diff(fn("entry:\n  %x = add i32 %a, 1\n  br label %mid\nmid:\n  %y = "
                 "load i32, ptr %p\n"
                 "  %z = mul i32 %y, %x\n  br label %end\nend:\n  ret i32 %z",
                 S),
              fn("entry:\n  %x = add i32 %a, 1\n  %y = load i32, ptr %p\n  %z "
                 "= mul i32 %y, %x\n"
                 "  br label %end\nend:\n  ret i32 %z",
                 S));
  const Change *C = t::find(D, "block", "merged");
  CHECK_MSG(C, t::dump(D));
  CHECK(C->Inferred);
  CHECK(t::hasDetail(C, "2 of 2"));
}

TEST(diff, split_block) {
  std::string S = "define i32 @f(ptr %p, i32 %a)";
  ModuleDiff D =
      t::diff(fn("entry:\n  %x = add i32 %a, 1\n  %y = load i32, ptr %p\n  %z "
                 "= mul i32 %y, %x\n"
                 "  br label %end\nend:\n  ret i32 %z",
                 S),
              fn("entry:\n  %x = add i32 %a, 1\n  br label %mid\nmid:\n  %y = "
                 "load i32, ptr %p\n"
                 "  %z = mul i32 %y, %x\n  br label %end\nend:\n  ret i32 %z",
                 S));
  CHECK_MSG(t::find(D, "block", "split"), t::dump(D));
}

TEST(diff, forwarding_block_removed) {
  ModuleDiff D = t::diff(
      fn("entry:\n  br label %fwd\nfwd:\n  br label %end\nend:\n  ret i32 %a"),
      fn("entry:\n  br label %end\nend:\n  ret i32 %a"));
  const Change *C = t::find(D, "block", "removed", "fwd");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "only a terminator"));
  CHECK(t::find(D, "cfg", "added", "edge %entry -> %end"));
}

static std::string diamond(const std::string &Cmp, bool Swap) {
  return fn("entry:\n  %c = icmp " + Cmp + " i32 %a, %b\n  br i1 %c, label %" +
            (Swap ? "r" : "l") + ", label %" + (Swap ? "l" : "r") +
            "\nl:\n  ret i32 1\nr:\n  ret i32 2");
}

TEST(diff, inverted_branch_condition_is_equivalent_control_flow) {
  ModuleDiff D = t::diff(diamond("slt", false), diamond("sge", true));
  const Change *C = t::find(D, "cfg", "modified", "branch condition");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "successors swapped"));
  CHECK(t::hasDetail(C, "control flow is equivalent"));
  CHECK_EQ(int(C->Significance), int(Sig::Minor));
}

TEST(diff, changed_branch_condition) {
  ModuleDiff D = t::diff(diamond("slt", false), diamond("sle", false));
  CHECK_MSG(t::find(D, "instruction", "", "predicate: slt -> sle"), t::dump(D));
}

TEST(diff, terminator_kind_change) {
  ModuleDiff D = t::diff(fn("entry:\n  %c = icmp eq i32 %a, 1\n  br i1 %c, "
                            "label %one, label %def\none:\n  ret i32 1\n"
                            "def:\n  ret i32 0"),
                         fn("entry:\n  switch i32 %a, label %def [ i32 1, "
                            "label %one ]\none:\n  ret i32 1\n"
                            "def:\n  ret i32 0"));
  const Change *C = t::find(D, "cfg", "modified", "terminator");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "br (2 successors) -> switch (2 successors)"));
}

TEST(diff, unreachable_block_added) {
  ModuleDiff D = t::diff(fn("entry:\n  ret i32 %a"),
                         fn("entry:\n  ret i32 %a\ndead:\n  ret i32 0"));
  const Change *C = t::find(D, "cfg", "unreachable");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "unreachable from entry in B"));
}

TEST(diff, loop_removed_and_topology) {
  std::string S = "define i32 @f(i32 %a)";
  ModuleDiff D = t::diff(fn("entry:\n  br label %loop\nloop:\n  %i = phi i32 [ "
                            "0, %entry ], [ %n, %loop ]\n"
                            "  %n = add i32 %i, 1\n  %c = icmp slt i32 %n, 4\n "
                            " br i1 %c, label %loop, label %exit\n"
                            "exit:\n  ret i32 %n",
                            S),
                         fn("entry:\n  br label %exit\nexit:\n  ret i32 4", S));
  CHECK_MSG(t::find(D, "loop", "removed"), t::dump(D));
  const Change *T = t::find(D, "cfg", "modified", "CFG topology");
  CHECK(T);
  CHECK(t::hasDetail(T, "blocks 3 -> 2"));
}

TEST(diff, loop_rotation_detected) {
  std::string S = "define void @f(i32 %n)";
  ModuleDiff D =
      t::diff(fn("entry:\n  br label %cond\ncond:\n  %i = phi i32 [ 0, %entry "
                 "], [ %inc, %body ]\n"
                 "  %c = icmp slt i32 %i, %n\n  br i1 %c, label %body, label "
                 "%exit\nbody:\n"
                 "  %inc = add i32 %i, 1\n  br label %cond\nexit:\n  ret void",
                 S),
              fn("entry:\n  %g = icmp slt i32 0, %n\n  br i1 %g, label %body, "
                 "label %exit\nbody:\n"
                 "  %i = phi i32 [ 0, %entry ], [ %inc, %body ]\n  %inc = add "
                 "i32 %i, 1\n"
                 "  %c = icmp slt i32 %inc, %n\n  br i1 %c, label %body, label "
                 "%exit\nexit:\n  ret void",
                 S));
  bool Rotated = false;
  for (auto *C : t::changes(D))
    if (C->Cat == "loop" && t::hasDetail(C, "rotated (latch exits): no -> yes"))
      Rotated = true;
  CHECK_MSG(Rotated, t::dump(D));
}

TEST(diff, signature_linkage_cc_and_attributes) {
  std::string Body = "entry:\n  ret i32 0";
  ModuleDiff D = t::diff(fn(Body, "define internal i32 @f(i32 %a) #0") +
                             "attributes #0 = { \"target-cpu\"=\"x86-64\" }\n",
                         fn(Body, "define hidden fastcc i32 @f(i64 %a) #0") +
                             "attributes #0 = { nounwind willreturn "
                             "\"target-cpu\"=\"skylake\" }\n");
  CHECK_MSG(t::find(D, "linkage", "modified", "linkage"), t::dump(D));
  CHECK(t::find(D, "linkage", "modified", "visibility"));
  CHECK(t::find(D, "signature", "modified", "calling-convention"));
  CHECK(t::find(D, "signature", "modified", "parameters"));
  const Change *A = t::find(D, "attribute", "modified");
  CHECK(A);
  CHECK(t::hasDetail(A, "+nounwind"));
  CHECK(t::hasDetail(A, "+willreturn"));
  const Change *Cfg = t::find(D, "config", "modified");
  CHECK(Cfg);
  CHECK(t::hasDetail(Cfg, "skylake"));
  DiffOptions O;
  O.CompareAttributes = false;
  CHECK(!t::find(
      t::diff(fn(Body), fn(Body, "define i32 @f(i32 %a, i32 %b) nounwind"), O),
      "attribute", ""));
}

TEST(diff, call_site_attributes) {
  std::string Decl = "declare i32 @g(i32)\n";
  ModuleDiff D =
      t::diff(Decl + fn("entry:\n  %r = call i32 @g(i32 %a)\n  ret i32 %r"),
              Decl + fn("entry:\n  %r = call i32 @g(i32 %a) #0\n  ret i32 %r") +
                  "attributes #0 = { nounwind }\n");
  const Change *C = t::find(D, "instruction", "", "call attributes: +nounwind");
  CHECK_MSG(C, t::dump(D));
}

TEST(diff, functions_added_removed_and_declarations) {
  ModuleDiff D = t::diff("define void @gone() {\n  ret void\n}\ndeclare void "
                         "@ext()\ndefine void @keep() {\n  ret void\n}\n",
                         "define void @keep() {\n  ret void\n}\ndefine void "
                         "@new() {\n  ret void\n}\n"
                         "declare i32 @llvm.smax.i32(i32, i32)\n");
  CHECK_MSG(t::find(D, "function", "removed", "@gone"), t::dump(D));
  CHECK(t::find(D, "function", "added", "@new"));
  CHECK(t::find(D, "declaration", "removed", "@ext"));
  CHECK(t::find(D, "intrinsic", "added", "@llvm.smax.i32"));
  unsigned Statuses = 0;
  for (auto &F : D.Funcs)
    Statuses += F.Status == "added" || F.Status == "removed";
  CHECK_EQ(Statuses, 2u);
}

TEST(diff, definition_becomes_declaration) {
  ModuleDiff D =
      t::diff("define void @f() {\n  ret void\n}\n", "declare void @f()\n");
  const Change *C = t::find(D, "function", "modified", "@f");
  CHECK_MSG(C, t::dump(D));
  CHECK_EQ(C->Before, std::string("definition"));
  CHECK_EQ(C->After, std::string("declaration"));
}

TEST(diff, global_variable_changes) {
  ModuleDiff D = t::diff(
      "@a = global i32 1, align 4\n@b = constant i32 2\n@gone = global i8 0\n",
      "@a = global i32 2, align 8\n@b = global i32 2\n@new = global i8 0\n");
  const Change *A = t::find(D, "global", "modified", "@a");
  CHECK_MSG(A, t::dump(D));
  CHECK(t::hasDetail(A, "initializer: i32 1 -> i32 2"));
  CHECK(t::hasDetail(A, "alignment: 4 -> 8"));
  CHECK(t::hasDetail(t::find(D, "global", "modified", "@b"),
                     "constant: constant -> global"));
  CHECK(t::find(D, "global", "removed", "@gone"));
  CHECK(t::find(D, "global", "added", "@new"));
}

TEST(diff, string_literal_renumbering_is_cosmetic) {
  std::string A =
      "@.str = private unnamed_addr constant [2 x i8] c\"a\\00\"\n"
      "@.str.1 = private unnamed_addr constant [2 x i8] c\"b\\00\"\n"
      "declare void @use(ptr)\n"
      "define void @f() {\n  call void @use(ptr @.str)\n  call void @use(ptr "
      "@.str.1)\n  ret void\n}\n";
  std::string B =
      "@.str = private unnamed_addr constant [2 x i8] c\"b\\00\"\n"
      "@.str.1 = private unnamed_addr constant [2 x i8] c\"a\\00\"\n"
      "declare void @use(ptr)\n"
      "define void @f() {\n  call void @use(ptr @.str.1)\n  call void @use(ptr "
      "@.str)\n  ret void\n}\n";
  ModuleDiff D = t::diff(A, B);
  CHECK_MSG(t::atLeast(D, Sig::Minor) == 0, t::dump(D));
  CHECK_EQ(t::count(D, "global", "renamed"), 2u);
}

TEST(diff, unmatched_global_with_colliding_name_is_not_equated) {
  std::string A =
      "@.str.1 = private constant [2 x i8] c\"a\\00\"\n"
      "declare void @use(ptr)\n"
      "define void @f() {\n  call void @use(ptr @.str.1)\n  ret void\n}\n";
  std::string B =
      "@.str = private constant [2 x i8] c\"a\\00\"\n"
      "@.str.1 = private constant [2 x i8] c\"z\\00\"\n"
      "declare void @use(ptr)\n"
      "define void @f() {\n  call void @use(ptr @.str.1)\n  ret void\n}\n";
  ModuleDiff D = t::diff(A, B);
  CHECK_MSG(t::find(D, "instruction", "operands-changed"), t::dump(D));
}

TEST(diff, internal_function_rename_is_matched_by_content) {
  std::string Body = "  %r = add i32 %x, 7\n  ret i32 %r\n}\n";
  ModuleDiff D = t::diff("define internal i32 @helper.1(i32 %x) {\n" + Body +
                             "define i32 @f(i32 %a) {\n  %v = call i32 "
                             "@helper.1(i32 %a)\n  ret i32 %v\n}\n",
                         "define internal i32 @helper.7(i32 %x) {\n" + Body +
                             "define i32 @f(i32 %a) {\n  %v = call i32 "
                             "@helper.7(i32 %a)\n  ret i32 %v\n}\n");
  const Change *C = t::find(D, "function", "renamed", "@helper.1 -> @helper.7");
  CHECK_MSG(C, t::dump(D));
  CHECK(C->Inferred);
  CHECK_MSG(t::count(D, "instruction") == 0, t::dump(D));
}

TEST(diff, ambiguous_function_matching_is_reported) {
  std::string Body = "(i32 %x) {\n  %r = add i32 %x, 7\n  ret i32 %r\n}\n";
  ModuleDiff D = t::diff(
      "define internal i32 @a" + Body + "define internal i32 @b" + Body,
      "define internal i32 @c" + Body + "define internal i32 @d" + Body);
  const Change *C = t::find(D, "match", "ambiguous");
  CHECK_MSG(C, t::dump(D));
  CHECK_EQ(C->Confidence, std::string("ambiguous"));
  CHECK_EQ(t::count(D, "function", "removed"), 2u);
}

TEST(diff, module_properties) {
  std::string A =
      "target datalayout = \"e-m:e-i64:64-n32:64\"\n"
      "target triple = \"x86_64-unknown-linux-gnu\"\n"
      "%struct.S = type { i32, i32 }\n@g = global %struct.S zeroinitializer\n"
      "!llvm.module.flags = !{!0}\n!0 = !{i32 8, !\"PIC Level\", i32 2}\n"
      "!llvm.ident = !{!1}\n!1 = !{!\"clang version 19.1.7\"}\n";
  std::string B =
      "target datalayout = \"e-m:e-i64:64-n32:64-S128\"\n"
      "target triple = \"aarch64-unknown-linux-gnu\"\n"
      "%struct.S = type { i32, i64 }\n@g = global %struct.S zeroinitializer\n"
      "!llvm.module.flags = !{!0}\n!0 = !{i32 8, !\"PIC Level\", i32 1}\n"
      "!llvm.ident = !{!1}\n!1 = !{!\"clang version 20.1.0\"}\n";
  ModuleDiff D = t::diff(A, B);
  CHECK_MSG(t::find(D, "module", "modified", "target triple"), t::dump(D));
  CHECK(t::find(D, "module", "modified", "data layout"));
  CHECK(t::find(D, "type", "modified", "%struct.S"));
  CHECK(t::find(D, "config", "modified", "PIC Level"));
  CHECK(t::find(D, "config", "modified", "!llvm.ident"));
  CHECK(D.Incompatible);
  bool Note = false;
  for (auto &N : D.Compat)
    Note |= N.find("different LLVM major versions") != std::string::npos;
  CHECK(Note);
}

TEST(diff, phi_incoming_value_change) {
  ModuleDiff D = t::diff(fn("entry:\n  br label %m\nm:\n  %p = phi i32 [ %a, "
                            "%entry ]\n  ret i32 %p"),
                         fn("entry:\n  br label %m\nm:\n  %p = phi i32 [ %b, "
                            "%entry ]\n  ret i32 %p"));
  const Change *C = t::find(D, "instruction", "operands-changed");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "incoming from %entry: i32 %a -> i32 %b"));
}

TEST(diff, rows_cover_both_sides) {
  ModuleDiff D = t::diff(fn("entry:\n  %x = add i32 %a, %b\n  ret i32 %x"),
                         fn("entry:\n  %x = add nsw i32 %a, %b\n  %y = mul i32 "
                            "%x, 2\n  ret i32 %y"));
  unsigned Mod = 0, Add = 0, Label = 0;
  for (auto &R : D.Funcs[0].Rows) {
    Mod += R.T == Row::Mod;
    Add += R.T == Row::Add;
    Label += R.T == Row::Label;
  }
  CHECK_EQ(Label, 1u);
  CHECK(Mod >= 1);
  CHECK(Add >= 1);
}

TEST(diff, canonical_names_make_unchanged_rows_identical) {
  DiffOptions O;
  O.NameMode = Names::Canonical;
  ModuleDiff D = t::diff(
      fn("entry:\n  %x = add i32 %a, %b\n  %y = mul i32 %x, 2\n  ret i32 %y"),
      fn("entry:\n  %s = add i32 %p, %q\n  %t = mul i32 %s, 3\n  ret i32 %t",
         "define i32 @f(i32 %p, i32 %q)"),
      O);
  bool SameRow = false;
  for (auto &R : D.Funcs[0].Rows)
    if (R.T == Row::Same && R.Left.find("add") != std::string::npos)
      SameRow = R.Left == R.Right;
  CHECK(SameRow);
}

TEST(diff, function_filter) {
  DiffOptions O;
  O.Functions = {"keep*"};
  ModuleDiff D = t::diff("define void @keep1() {\n  ret void\n}\ndefine void "
                         "@x() {\n  ret void\n}\n",
                         "define void @keep1() {\n  unreachable\n}\n", O);
  CHECK_EQ(D.Funcs.size(), 1u);
  CHECK_EQ(D.Funcs[0].NameA, std::string("@keep1"));
}

TEST(diff, large_function_is_handled) {
  std::string A = "define i32 @big(i32 %a) {\nentry:\n", B = A;
  std::string Prev = "%a";
  for (int I = 0; I < 4000; ++I) {
    std::string V = "%v" + std::to_string(I);
    A += "  " + V + " = add i32 " + Prev + ", " + std::to_string(I) + "\n";
    B += "  " + V + " = add i32 " + Prev + ", " +
         std::to_string(I % 997 == 0 ? I + 1 : I) + "\n";
    Prev = V;
  }
  A += "  ret i32 " + Prev + "\n}\n";
  B += "  ret i32 " + Prev + "\n}\n";
  ModuleDiff D = t::diff(A, B);
  CHECK_EQ(t::count(D, "instruction", "operands-changed"), 5u);
  CHECK_EQ(D.Funcs[0].Stats.Unchanged, 3996u);
}

TEST(diff, redundant_instruction_merged) {
  std::string S = "define i32 @f(ptr %p)";
  ModuleDiff D = t::diff(
      fn("entry:\n  %x = load i32, ptr %p\n  %y = load i32, ptr %p\n"
         "  %z = add i32 %x, %y\n  ret i32 %z",
         S),
      fn("entry:\n  %x = load i32, ptr %p\n  %z = add i32 %x, %x\n  ret i32 %z",
         S));
  const Change *C = t::find(D, "instruction", "merged");
  CHECK_MSG(C, t::dump(D));
  CHECK(C->Inferred);
  CHECK(t::hasDetail(C, "1 of 1 use(s) now read the remaining"));
}

TEST(diff, duplicated_instruction_split) {
  std::string S = "define i32 @f(i32 %a, i1 %c)";
  ModuleDiff D = t::diff(
      fn("entry:\n  %x = mul i32 %a, 3\n  br i1 %c, label %l, label %r\nl:\n"
         "  ret i32 %x\nr:\n  ret i32 %x",
         S),
      fn("entry:\n  %x = mul i32 %a, 3\n  br i1 %c, label %l, label %r\nl:\n"
         "  ret i32 %x\nr:\n  %x2 = mul i32 %a, 3\n  ret i32 %x2",
         S));
  const Change *C = t::find(D, "instruction", "split");
  CHECK_MSG(C, t::dump(D));
  CHECK(t::hasDetail(C, "duplicates %x = mul i32 %a, 3"));
}
