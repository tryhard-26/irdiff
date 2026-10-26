# Changelog

All notable changes are listed here. Versions follow semantic versioning;
the JSON output carries its own `schema` number.

## 2.0.0 - 2026-10-26

- `passes` no longer fails with "could not read back a pass snapshot" when
  a pass adds metadata that names functions, such as `!callees` from
  CalledValuePropagation; such metadata is now remapped in snapshots.
- Instruction flags are read from the instruction rather than its printed
  text, so flags of constant-expression operands are no longer reported as
  instruction flag changes, and real changes they masked are reported.
- Commuted operands with different fast-math flags are no longer reported
  as equivalent.
- Positional matching ignores flags, so `add` and `add nuw`, or `call` and
  `tail call`, of the same operation are paired instead of reported as a
  removal and an addition.
- Constant GEPs rewritten to the same base, flags and byte offset (for
  example struct indices to `i8` offsets) are reported as `rewritten` and
  cosmetic.
- `investigate` no longer reports removing restrictive attributes such as
  `noinline` or `optnone` as lost attributes, and caps evidence lines at
  240 characters.
- Diffing is 3-6x faster on large modules (SQLite: 15.5 s to 3.1 s for
  O2 vs O3): each function is printed once, metadata text is cached per
  node, and block matching skips pairs whose similarity bound cannot reach
  the threshold.

## 1.0.0 - 2026-10-13

First release.

- `diff`: semantic comparison of `.ll` and `.bc` snapshots with
  normalization of names, labels, metadata and attribute-group numbering,
  layout, commutative operands, mirrored compares, phi and switch ordering,
  and auto-numbered private globals.
- Function, block and instruction matching with CFG propagation, multi-round
  operand-aware alignment, cross-block matching and SSA def-use
  propagation; ambiguous matches are reported.
- Classification of opcode, operand, flag, type, predicate, alignment,
  attribute and metadata changes; moved, reordered, merged and split blocks
  and instructions; CFG edges, terminators, reachability and topology;
  LoopInfo-based loop comparison; function, global, declaration, intrinsic
  and module property changes.
- `passes`: per-pass attribution on the new pass manager with remark
  capture and snapshot export; replay of `-print-after-all` and
  `-print-changed` logs.
- `pipelines`: comparison of two pipelines or target configurations,
  including remarks and pass sequence differences.
- `analyze`, `--analyses` and `investigate`: dominator, loop, SCEV, alias
  analysis and MemorySSA facts and differences, remark files, and
  evidence-backed hints.
- Text, unified, side-by-side and JSON output; filters; stable exit codes.
- Supports building against LLVM 19 through 23.
