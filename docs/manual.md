# irdiff manual

- [Commands](#commands)
- [Options](#options)
- [Changes: categories, kinds, significance](#changes)
- [Normalization rules](#normalization-rules)
- [Matching and confidence](#matching-and-confidence)
- [Analyses](#analyses)
- [Optimization remarks](#optimization-remarks)
- [Pass attribution](#pass-attribution)
- [Output formats](#output-formats)
- [Compatibility reporting](#compatibility-reporting)
- [Reproducible examples](#reproducible-examples)
- [Limitations](#limitations)

## Commands

| Command | Inputs | Purpose |
|---|---|---|
| `diff` (default) | `A B` | Compare two snapshots. `irdiff A B` is the same as `irdiff diff A B`. |
| `investigate` | `A B` | `diff` with all analyses and hints enabled. |
| `passes` | `IN` or `--from-log LOG` | Run a pipeline (or replay a log) and attribute each change to one pass execution. |
| `pipelines` | `IN` | Run pipeline `-a` and pipeline `-b` on separate copies of `IN` and compare results, remarks and pass sequences. |
| `analyze` | `IN` | Print dominator, loop, SCEV, alias and MemorySSA facts per function. |

Any input may be `-` for standard input (only one per command). Inputs are
detected by content, not extension: bitcode (including the wrapper format),
textual IR, and clear errors for object files, archives, MIR and assembly.

## Options

Output

| Option | Meaning |
|---|---|
| `-f, --format=text\|unified\|side-by-side\|json` | Output format (default `text`). |
| `-o FILE` | Write output to `FILE`. |
| `-v, --verbose` | All details, significance labels, full side-by-side listings, and per-row data in JSON. |
| `-q, --quiet` | No output; exit status only. |
| `--summary` | Only summary statistics (text), or only per-pass summaries (`passes`). |
| `-U, --context=N` | Context lines for `unified` and `side-by-side` (default 3). |
| `--width=N` | Width for `side-by-side` (default `$COLUMNS`, else 160). |
| `--names=original\|canonical` | Show values with their names from the inputs, or with shared normalized names (`%vN`, `%bbN`) so unchanged lines read identically on both sides. |
| `--color[=auto\|always\|never]`, `--no-color` | Colors for text output. `auto` honours `NO_COLOR` and `TERM=dumb`. |
| `--fail-on=any\|minor\|major\|never` | Which shown changes make the exit status `1` (default `any`). |
| `--hints` | Add the "potentially relevant differences" section (`investigate` always does). |

Filtering (applied after comparison; filtered changes are counted as hidden)

| Option | Meaning |
|---|---|
| `-F, --function=GLOB` | Functions to compare (repeatable; `@` prefix optional). Also filters remarks and pass steps. |
| `--block=GLOB` | Changes located in matching blocks. |
| `--category=LIST` | Change categories, comma-separated. |
| `--change=LIST` | Change kinds, comma-separated. |
| `--pass=GLOB` | Pass steps whose class name (`LICMPass`) or pipeline name (`licm`) matches. |
| `--analysis=LIST` | Analyses to run and report: `dom,loops,scev,aa,mssa`. |
| `--min-significance=cosmetic\|minor\|major` | Default `minor`. `--all` = `cosmetic`, `--major` = `major`. |
| `--list-categories` | Print the accepted categories, kinds and analyses. |

Normalization

| Option | Meaning |
|---|---|
| `--debug-info` | Compare debug locations, debug variable records and debug module flags. |
| `--ignore-metadata` | Ignore non-debug metadata attachments (`!tbaa`, `!range`, `!llvm.loop`, ...). |
| `--ignore-attributes` | Ignore function, parameter and call-site attribute differences. |
| `--strict` | Report block layout and `source_filename` changes as minor instead of cosmetic. |

Input and pipelines

| Option | Meaning |
|---|---|
| `--no-verify` | Skip the IR verifier on inputs. |
| `--require-compatible` | Exit `4` if target triples or data layouts differ. |
| `--analyses` | `diff`: compare analysis results too. |
| `--aa-limit=N` | Loads/stores per function considered for alias queries (default 48). |
| `--remarks-a=FILE`, `--remarks-b=FILE` | Remarks for each side (YAML or bitstream, as written by `-fsave-optimization-record`). |
| `-p PIPELINE` | Pipeline in `opt -passes=` syntax (default `default<O2>`). |
| `-O0`..`-O3`, `-Os`, `-Oz` | Shorthand for `-p 'default<ON>'`. |
| `-a PIPELINE`, `-b PIPELINE` | `pipelines`: pipeline for each side (defaults `default<O1>`, `default<O2>`). |
| `--mcpu`, `--mattr` | Target CPU and features for the TargetMachine. |
| `--mcpu-b`, `--mattr-b` | `pipelines`: override for side B. |
| `--no-target` | Do not create a TargetMachine; use target-independent cost models. |
| `--verify-each-pass` | Run the verifier after each pass. |
| `--save-snapshots=DIR` | `passes`: write `0000-input.ll` and the module after every changing pass. |
| `--from-log=FILE` | `passes`: replay a log instead of running a pipeline. |

## Changes

Each reported change has:

- **category**: `module`, `type`, `global`, `declaration`, `intrinsic`,
  `function`, `signature`, `linkage`, `attribute`, `config`, `block`,
  `instruction`, `cfg`, `loop`, `metadata`, `debug`, `analysis`, `remark`,
  `pass`, `match`, `hint`.
- **kind**: `added`, `removed`, `modified`, `opcode-changed`,
  `operands-changed`, `flags-changed`, `type-changed`, `metadata-changed`,
  `moved`, `reordered`, `commuted`, `merged`, `split`, `renamed`,
  `unreachable`, `ambiguous`, `observation`.
- **significance**:
  - `cosmetic`: no semantic effect (renames, layout moves, commuted operands,
    reordering of instructions without side effects or memory access,
    renumbered private globals).
  - `minor`: unlikely to change generated code, or not semantic by itself:
    metadata, debug info, compiler configuration attributes and module flags,
    `dso_local`/`unnamed_addr`, ambiguity notes.
  - `major`: everything else: instructions, operands, flags, CFG edges and
    terminators, loops, signatures, linkage, attributes, globals, alias and
    SCEV changes, missed/passed remarks, pipeline structure.

Instruction classification for a matched pair, in order: opcode change; only
metadata differs; otherwise a list of details (result type, predicate, flags
such as `nsw`/`nuw`/`exact`/`inbounds`/`nneg`/`disjoint`/fast-math/`tail`,
alignment, call-site attributes, each differing operand, phi incoming values
per predecessor block). If exactly one detail class applies it becomes the
kind (`flags-changed`, `operands-changed`, `type-changed`), otherwise
`modified`.

Instruction merges and duplicates: a removed instruction that computed the
same expression as one that remains is reported as `merged` (with how many
of its uses now read the survivor, the typical CSE/GVN result); an added
instruction identical to an existing one is reported as `split` (a
duplicate, for example from tail duplication). Both are `[inferred]`.

Block changes: `added`/`removed`; `merged` when most non-terminator
instructions of a removed block now live in one block of B; `split` for the
reverse; `moved` when the layout order changed; `renamed` for label-only
differences (cosmetic). A removed block that held only a terminator is
reported as such (typical SimplifyCFG cleanup).

CFG changes: added and removed edges between matched blocks, terminator kind
or successor count changes, branch condition changes (with the defining
instructions), swapped successors (noted as equivalent control flow when the
condition's predicate is inverted at the same time), blocks that became
reachable or unreachable from entry, and a topology summary (blocks, edges,
cyclomatic complexity, critical edges, return blocks).

Loop changes (LoopInfo on both sides; loops are matched by header or by a
majority of their blocks): added/removed loops, header, depth, block count,
exit blocks, latches, preheader presence, rotated form, loop-simplify form,
sub-loops and `!llvm.loop` metadata.

Function changes: linkage, visibility, DLL storage, `dso_local`,
`unnamed_addr`, calling convention, return and parameter types, return,
parameter and function attributes (with per-attribute `+`/`-` deltas),
string attributes such as `target-cpu` and `target-features` (category
`config`), section, alignment, GC, personality, comdat, prefix/prologue data,
function metadata, definition/declaration status, and renames.

Module changes: triple, data layout, `source_filename` (cosmetic), module
inline asm, module flags (`config`, or `debug` for debug-info flags), named
metadata (`llvm.ident` and `llvm.commandline` count as `config`), identified
struct types, global variables, aliases and ifuncs (field by field),
declarations and intrinsics.

## Normalization rules

Ignored, because they carry no semantics:

- Names of local values, arguments and blocks; slot numbering (`%5`).
- Metadata slot numbers (`!12`) and attribute-group numbers (`#3`).
  Metadata and attributes are compared by content instead.
- Block layout order (reported as cosmetic `moved`).
- Operand order of commutative instructions with identical flags.
- `icmp`/`fcmp` with swapped operands and the mirrored predicate.
- Order of phi incoming entries and of `switch` cases.
- Order of instructions without side effects or memory access inside a block
  (cosmetic `reordered`). Reordering of memory operations is `major`.
- Numbering of private/internal auto-named globals and functions (`.str.3`
  vs `.str.7`, `f.llvm.123`): they are matched by content.
- Debug locations, debug intrinsics/records and debug module flags (unless
  `--debug-info`), including `DILocation` operands inside loop metadata.
- `source_filename`.

Never treated as equivalent:

- Any difference in opcode, type, flags, predicate, alignment, volatility,
  atomic ordering, call target, calling convention, or call/parameter
  attributes.
- Operand differences other than the commutative cases above.
- Metadata content differences (`!tbaa`, `!range`, `!noalias`,
  `!llvm.loop`, ...) unless `--ignore-metadata`.
- Exported (non-local) symbol names: a renamed external function is reported
  as one removed and one added function.

## Matching and confidence

Matching is described in the README. Confidence labels on changes:

| Label | Meaning |
|---|---|
| (none) | The pair was aligned in its block, was the only candidate with identical content, or was matched through an SSA use with the same opcode. |
| `positional-match` | Paired by position and opcode within the same gap between aligned neighbours, or by content alignment after a merge/split. |
| `inferred-match` | Paired through a def-use edge with a different opcode of the same class (binary op, cast, compare), or moved across blocks by position. |
| `ambiguous` | A block or global had a competing candidate within 5% of the chosen score; the match notes list them. |
| `[inferred]` | The change itself is an interpretation (block merge/split, content-based rename, hint). |

## Analyses

`--analyses`, `investigate` and `analyze` use LLVM's new pass manager
analyses with the default AA pipeline (BasicAA, scoped-noalias, TBAA, ...).

| Name | Facts | Compared as |
|---|---|---|
| `dom` | Immediate dominator and depth of each block | idom of matched blocks changed |
| `loops` | Loops, depth, latches, exits, preheader, LCSSA and simplify form, loop metadata | (also part of every diff) |
| `scev` | Constant and maximum trip count, backedge-taken count | per matched loop |
| `aa` | Alias result for load/store pairs (not load/load) | per matched pair, e.g. `NoAlias -> MayAlias` |
| `mssa` | Clobbering memory access of each load | per matched load, e.g. `liveOnEntry -> store ...` |

These are facts computed by LLVM on each snapshot. irdiff does not claim a
causal link between an analysis change and a transformation.

## Optimization remarks

- `diff`/`investigate` read remarks with `--remarks-a/--remarks-b`
  (`clang -fsave-optimization-record`, YAML or bitstream).
- `passes` and `pipelines` capture remarks live, for every pass, and record
  which pass execution emitted them.

Remarks are grouped by (function, pass, remark name, type, location) and
compared by message. Messages are reproduced verbatim as LLVM's statements,
under "optimization remarks (as reported by LLVM)".

Hints (`--hints`, `investigate`) are listed under "potentially relevant
differences": missed remarks present only in B, passed remarks lost in B,
weaker alias results, less precise trip counts, dropped poison flags, lost
attributes, new calls, removed loops, TBAA differences and pipeline
differences. Each hint lists its evidence and states what it may affect. A
hint is a pointer for investigation, not a diagnosis.

## Pass attribution

`irdiff passes IN -p PIPELINE`:

1. Builds a TargetMachine from the module triple (unless `--no-target`) so
   cost models match the target, then the new pass manager with standard
   instrumentation (`optnone` handling, `--verify-each-pass`).
2. Before and after every pass execution (pass managers and adaptors are
   skipped) hashes the IR unit the pass ran on (function, loop's function,
   SCC functions, or module) plus module-level counts. The hash covers
   operands, flags, types, alignment, metadata attachments and call
   attributes.
3. Each defined function is copied into a scratch module (with declarations
   of the globals it references) and kept as bitcode. When a pass changes a
   function, a new copy is taken and compared with the previous one, so the
   cost of a step is proportional to the changed function, not the module.
   Debug records are not copied unless `--debug-info` is given.
4. When a module-level pass changes globals, or a pass adds or deletes
   functions or declarations, module-level state is re-read from a bitcode
   snapshot of the whole module and compared.
5. Pass managers and adaptors are watched too: when they change the module
   between passes (for example deleting functions that became dead after
   inlining), the step is attributed to the adaptor.

The report lists every changing pass execution with a summary; `-v` prints
the full changes, `-f unified` prints unified diffs, `-f json` includes every
execution (`executions`), every changing step (`steps`) and all remarks.

`--from-log` accepts logs from `opt` or `clang -mllvm`:

```sh
opt -passes='default<O2>' -print-after-all -print-module-scope in.ll -o /dev/null 2> log.txt
clang -O2 -c x.c -mllvm -print-changed -mllvm -print-module-scope 2> log.txt
```

Sections are recognized by their `*** IR Dump ...` headers (`After`,
`Before`, `At Start`, and the `omitted because no change`/`ignored` variants
of `-print-changed`). Consecutive module snapshots are diffed and attributed
to the pass named in the header. Without a `Before`/`At Start` snapshot the
first pass cannot be attributed; irdiff notes this.

## Output formats

**text**: sections for module changes, each changed function (with block and
instruction counts), remarks, pipeline differences, hints, and a summary.
Each change is one line `SYMBOL [category] subject`, optionally followed by
`=> B text` and indented details. Symbols: `+` added, `-` removed, `~`
modified, `>` moved/reordered, `*` merged/split, `=` renamed/equivalent,
`!` reachability, `?` ambiguous, `.` observation.

**unified**: `--- A` / `+++ B` headers, then one `@@ define @f @@` hunk
group per changed function in matched order. Moved and reordered
instructions carry `; moved to %bb` comments. Module-level changes come
first as an `@@ module @@` hunk.

**side-by-side**: the same rows in two columns; `<` removed, `>` added,
`|` modified.

**json**: one object, stable key order, no timestamps or addresses:

```json
{
  "tool": {"name": "irdiff", "version": "1.0.0", "llvm": "22.1.4", "schema": 1},
  "command": "diff",
  "inputs": {"a": {"path": "...", "format": "text", "producer": "", "triple": "...",
                   "data_layout": "...", "functions": 2, "declarations": 0,
                   "globals": 0, "instructions": 3}, "b": {}},
  "compatibility": {"incompatible": false, "notes": []},
  "summary": {"functions_compared": 2, "identical": 1, "equivalent": 0, "modified": 1,
              "added": 0, "removed": 0, "changes": 1, "hidden": 1,
              "max_significance": "major",
              "by_significance": {"major": 1, "minor": 0, "cosmetic": 0},
              "by_category": {"instruction": 1}, "by_kind": {"flags-changed": 1}},
  "module_changes": [],
  "functions": [
    {"name_a": "@inc", "name_b": "@inc", "status": "modified", "ambiguous": false,
     "stats": {"blocks_a": 1, "blocks_b": 1, "instructions_a": 2, "instructions_b": 2,
               "added": 0, "removed": 0, "modified": 1, "moved": 0, "unchanged": 1},
     "changes": [
       {"category": "instruction", "kind": "flags-changed", "significance": "major",
        "function": "@inc", "block": "entry", "subject": "%r = add i32 %x, 1",
        "before": "%r = add i32 %x, 1", "after": "%r = add nsw i32 %x, 1",
        "details": ["flags: +nsw"], "inferred": false}]}
  ],
  "remarks": [],
  "pipeline": [],
  "hints": []
}
```

Function `status` is one of `identical`, `equivalent` (only cosmetic
changes), `modified`, `added`, `removed`. With `-v`, each function also has
`rows` (`tag`: `same`, `removed`, `added`, `modified`, `label`; `a`, `b`,
`note`). `passes -f json` has `input`, `pipeline`, `pipeline_expanded`,
`notes`, `executions` (`index`, `pass`, `name`, `unit`, `changed`), `steps`
(the fields above plus `changed_functions` and `emitted_remarks`) and
`remarks`. The `schema` number changes only on incompatible changes to this
layout.

## Compatibility reporting

For each input irdiff records the format, the producer (bitcode identification
block, or `llvm.ident`), triple, data layout and size. It then notes:

- inputs produced by a newer LLVM than irdiff's;
- inputs from different LLVM major versions (auto-upgrade may hide or add
  differences);
- different formats (text vs bitcode);
- different target triples or data layouts (marked incompatible; exit `4`
  with `--require-compatible`);
- multi-module bitcode (only the first module is compared) and stripped
  invalid debug info.

Unparseable input from a newer producer exits with `4` and a note naming both
versions; other parse and verifier errors exit with `3` and LLVM's message
with file, line and column.

## Reproducible examples

All inputs are in the repository. `tests/fixtures/real/` was produced by
clang and opt 19.1.7 from `tests/fixtures/src/kernels.c` with
`--target=x86_64-unknown-linux-gnu -fno-discard-value-names -fno-ident`.

```sh
F=tests/fixtures
irdiff $F/rename_a.ll $F/rename_b.ll            # renamed, reordered, commuted: exit 0
irdiff $F/rename_a.ll $F/rename_b.ll --all      # show the cosmetic changes
irdiff $F/flags_a.ll $F/flags_b.ll -f unified   # one flag added
irdiff $F/real/kernels.O0.ll $F/real/kernels.O2.ll --summary
irdiff $F/real/kernels.O1.ll $F/real/kernels.O2.ll -F scale \
       --remarks-a $F/real/kernels.O1.opt.yaml --remarks-b $F/real/kernels.O2.opt.yaml --hints
irdiff investigate $F/alias_a.ll $F/alias_b.ll  # noalias dropped: AA and MemorySSA change
irdiff passes $F/real/kernels.O0.ll -O2 -F scale_restrict --pass=licm
irdiff passes --from-log $F/real/kernels.passes.log
irdiff pipelines $F/real/kernels.O0.ll -a 'default<O1>' -b 'default<O2>'
irdiff analyze $F/loops.ll
```

## Limitations

- Matching is heuristic. For heavily restructured code the result is the
  best pairing the structure supports; uncertainty is labelled.
- Hidden or filtered changes still count in `hidden`; exit status depends
  only on shown changes.
- Struct types are compared by name; a struct renamed by linking
  (`%struct.S.0`) appears as removed and added.
- `--debug-info` compares locations and variable records, not complete
  debug metadata graphs.
- Alias queries are pairwise and capped by `--aa-limit`.
- `passes --save-snapshots` prints the whole module after each changing pass;
  on very large modules restrict it with `--function`.
- If a function takes the address of a block in another function, `passes`
  falls back to whole-module snapshots for the run and says so.
- Attribute changes that LLVM discards when IR is written and read back (for
  example extra attributes on intrinsic declarations) are not reported.
- irdiff reads IR its linked LLVM version can read. Use an irdiff built
  against the producing LLVM, or a newer one, for new IR features.
