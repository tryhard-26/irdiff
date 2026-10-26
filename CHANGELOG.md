# Changelog

All notable changes are listed here. Versions follow semantic versioning;
the JSON output carries its own `schema` number.

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
