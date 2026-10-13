# irdiff

`irdiff` compares LLVM IR and explains what changed between two snapshots:
instructions, basic blocks, control flow, loops, attributes, globals and
module properties. It can also run a pass pipeline and attribute every change
to the pass that made it, compare two pipelines or target configurations, and
put optimization remarks and analysis results (alias analysis, MemorySSA,
dominators, loops, SCEV) next to the IR differences.

It reads textual IR (`.ll`) and bitcode (`.bc`), links against LLVM 19 to 23,
and runs on macOS and Linux.

```
$ irdiff passes kernels.O0.ll -O2 --function=scale_restrict --pass=licm
irdiff passes: kernels.O0.ll  pipeline: default<O2>
472 pass execution(s), 89 changed the IR, 1 shown after filters

#224 LICMPass (licm) on loop %for.body in @scale_restrict: instruction moved=1; 1 remark(s)
  @scale_restrict modified
    > [instruction] in %for.body: %0 = load float, ptr %k, align 4
        from %for.body (loop depth 1) to %for.body.lr.ph (loop depth 0)
        now outside 1 enclosing loop(s)
    remark [Passed] licm/Hoisted in @scale_restrict: hoisting load
```

## Install

### Homebrew (macOS, Linux)

```sh
brew tap tryhard-26/irdiff https://github.com/tryhard-26/irdiff
brew install tryhard-26/irdiff/irdiff
```

The formula builds against Homebrew's `llvm`.

### Prebuilt binaries

Each [release](https://github.com/tryhard-26/irdiff/releases) has
self-contained archives (LLVM linked statically) for:

| Platform | Archive |
|---|---|
| Linux x86_64 (glibc 2.35+) | `irdiff-<version>-linux-x86_64.tar.gz` |
| Linux aarch64 (glibc 2.35+) | `irdiff-<version>-linux-aarch64.tar.gz` |
| macOS 15+ arm64 | `irdiff-<version>-macos-arm64.tar.gz` |
| macOS 15+ x86_64 | `irdiff-<version>-macos-x86_64.tar.gz` |

```sh
V=1.0.0; P=linux-x86_64
curl -LO https://github.com/tryhard-26/irdiff/releases/download/v$V/irdiff-$V-$P.tar.gz
curl -LO https://github.com/tryhard-26/irdiff/releases/download/v$V/SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS        # macOS: shasum -a 256 --ignore-missing -c SHA256SUMS
tar xzf irdiff-$V-$P.tar.gz
sudo install irdiff-$V-$P/bin/irdiff /usr/local/bin/
```

Release binaries link LLVM 22 and initialize the X86, AArch64, ARM, RISCV
and WebAssembly targets. Other triples still work; pass pipelines then use
target-independent cost models and irdiff says so.

### From source

Requirements: CMake 3.20+, a C++17 compiler, and LLVM 19-23 development
files. `LLVM_DIR` is the directory holding `LLVMConfig.cmake`.

```sh
# Debian/Ubuntu/Raspberry Pi OS
sudo apt install cmake ninja-build llvm-19-dev zlib1g-dev libzstd-dev
LLVM_DIR=$(llvm-config-19 --cmakedir)
# macOS
brew install cmake ninja llvm
LLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR="$LLVM_DIR"
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build            # bin/irdiff and the man page
```

Useful options: `-DIRDIFF_STATIC_LLVM=ON` links LLVM component archives,
`-DIRDIFF_TARGETS="X86;AArch64"` limits initialized targets,
`-DIRDIFF_COVERAGE=ON` instruments for clang source-based coverage. On
memory-constrained machines such as a 1 GB Raspberry Pi, build with
`cmake --build build -j1`.

## Usage

```
irdiff [diff] A.ll B.ll          compare two snapshots (the default command)
irdiff passes in.ll -O2          run a pipeline, attribute each change to a pass
irdiff passes --from-log log.txt replay clang/opt -print-after-all output
irdiff pipelines in.ll -a P -b Q run two pipelines or configurations and compare
irdiff analyze in.ll             report dominator, loop, SCEV, alias, MemorySSA facts
irdiff investigate A.ll B.ll     diff + analyses + remarks + evidence-backed hints
```

Common examples:

```sh
irdiff before.ll after.ll                         # readable report
irdiff before.ll after.ll -f unified -U 5         # unified diff of the IR
irdiff before.ll after.ll -f side-by-side         # two columns
irdiff before.ll after.ll -f json > diff.json     # for scripts and CI
irdiff a.ll b.ll --function='vec_*' --category=cfg,loop
irdiff a.ll b.ll --major --fail-on=major          # CI gate on significant changes only
irdiff a.bc b.bc --analyses                       # add AA/MemorySSA/SCEV/dominator diffs
irdiff passes x.ll -p 'function(sroa,instcombine)' --save-snapshots snaps/
irdiff pipelines x.ll -a 'default<O2>' -b 'default<O3>' --mcpu=skylake
irdiff pipelines x.ll -a 'default<O2>' -b 'default<O2>' --mcpu=x86-64 --mcpu-b=znver4
irdiff investigate o2.ll o3.ll --remarks-a o2.opt.yaml --remarks-b o3.opt.yaml -F hot_loop
clang -O2 -S -emit-llvm x.c -mllvm -print-after-all -mllvm -print-module-scope 2> log.txt
irdiff passes --from-log log.txt --function=main
```

Every change has a category (`instruction`, `cfg`, `loop`, `attribute`, ...),
a kind (`added`, `removed`, `flags-changed`, `moved`, `merged`, ...) and a
significance (`cosmetic`, `minor`, `major`). `irdiff --list-categories` lists
them. Cosmetic changes (renames, block layout, commuted operands) are hidden
unless `--all` is given.

Exit status: `0` no differences shown, `1` differences shown, `2` usage error,
`3` unreadable or invalid input, `4` incompatible input (for example bitcode
from a newer LLVM, or differing targets with `--require-compatible`), `5`
internal or pipeline failure. `--fail-on=any|minor|major|never` sets which
differences produce `1`. `-q` prints nothing.

The full reference, including the JSON schema and the normalization rules, is
in [docs/manual.md](docs/manual.md) and `man irdiff`.

## How it works

1. **Load and check.** Inputs are parsed with LLVM's own reader and verified.
   Bitcode producer strings and `llvm.ident` are read so that version
   mismatches can be reported (or explained when parsing fails).
2. **Canonicalize.** Every local value and block is renamed to a positional
   token and each instruction is printed once with LLVM's printer. Metadata
   slot numbers, attribute-group numbers and debug attachments are stripped
   and compared separately by content. This makes the comparison independent
   of value names, block labels and `!N`/`#N` numbering while keeping every
   semantic detail the printer knows about.
3. **Match globals.** Functions and globals are paired by name, except
   auto-numbered local symbols (`.str.4`, `foo.llvm.123`, unnamed globals),
   which are paired by content so that renumbering does not show up as
   changes. Distinct candidates with identical content are reported as
   ambiguous instead of being guessed.
4. **Match blocks.** Entry blocks first, then blocks with unique content,
   then equal labels, then propagation along CFG edges, then a scored
   similarity pass. Close alternatives are flagged as ambiguous.
5. **Match instructions.** Within each block pair, an LCS alignment over
   instruction shapes is refined over several rounds as operand
   correspondences become known. Leftover instructions are matched across
   blocks when their operands already correspond (hoisting, sinking, merged
   and split blocks), and through SSA def-use edges: if a matched user reads
   an unmatched value in the same operand slot on both sides, those values
   correspond.
6. **Explain.** Matched pairs are classified (opcode, operands, flags,
   predicate, type, alignment, call attributes, metadata). Commuted operands,
   mirrored compares, reordered phi incoming values and switch cases are
   recognized as equivalent. Removed instructions that duplicated a surviving
   one are reported as merged, added copies as duplicates. CFG edges,
   terminators, reachability, loop structure (LoopInfo), and function,
   global and module properties are compared separately.

Anything derived from a heuristic rather than read directly from the IR is
marked `[inferred]` (block merges and splits, content-based renames) or with a
match confidence (`[positional-match]`, `[inferred-match]`, `[ambiguous]`).
Optimization remarks are quoted as LLVM reported them. `investigate` and
`--hints` list differences that are commonly relevant to missed optimizations
together with the evidence; they never state a cause.

`passes` instruments the new pass manager. Before and after every pass it
hashes the IR unit the pass ran on. When a pass changes a function, only that
function (with declarations of what it references) is copied and compared
with its previous copy; module-level state is re-read only when a module pass
changes globals. Each change is attributed to exactly one pass execution,
including dead-function cleanup that pass-manager adaptors do between passes.
Remarks emitted during a pass are attached to it.

Measured on an Apple M-series laptop with a Release build: comparing two
26 MB modules (49,000 instructions, full debug info) takes 1.2 seconds,
mostly parsing; tracing `default<O2>` on a 3,240-function module (265,000
pass executions, 10,900 of them changing the IR) takes 19 seconds.

## LLVM compatibility

| | |
|---|---|
| Build against | LLVM 19, 20, 21, 22, 23 (tested in CI on each) |
| Release binaries | one pinned LLVM version for all platforms (LLVM 22 for 1.0.x), recorded in each archive's `BUILDINFO` |
| Reads | IR that the linked LLVM can read: the same or older LLVM versions |

When an input cannot be parsed and its producer is newer than irdiff's LLVM,
irdiff exits with `4` and names both versions. When inputs come from
different LLVM versions, the report says so: LLVM's auto-upgrade can hide or
introduce differences in that case. Support for a new LLVM release is added
once it is published on apt.llvm.org; the oldest version is dropped when it
falls more than four releases behind.

## Limitations

- Matching is heuristic. Heavily restructured functions (aggressive
  unrolling, vectorization, outlining) are matched as well as the structure
  allows; uncertain pairings are labelled, not hidden.
- Debug info is ignored unless `--debug-info` is given; then debug locations
  and variable records are compared, not full DWARF metadata graphs.
- Identified struct types are compared by name and body.
- Alias queries are limited to the first `--aa-limit` (48) loads and stores per
  function.
- `--save-snapshots` prints the whole module after each changing pass, which
  is slow for very large modules; combine it with `--function`.
- Log replay (`--from-log`) needs module-scoped dumps
  (`-print-module-scope`).

## Development

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR="$LLVM_DIR"
cmake --build build && ctest --test-dir build --output-on-failure
```

The test suite (`tests/`) has unit tests for the matcher and every change
class, malformed-input tests, CLI tests that check exit codes and output
formats, and tests against real clang/opt output from LLVM 19 in
`tests/fixtures/real/` (regenerate with `tests/fixtures/src/kernels.c`).
CI also enforces `clang-format` and a line-coverage floor.

## Releases

Pushing a `vX.Y.Z` tag runs `.github/workflows/release.yml`. It builds static
binaries for the four platforms, runs the test suite on each, smoke-tests the
packaged archive, publishes the release with `SHA256SUMS`, points
`Formula/irdiff.rb` at the new tag and then installs and tests the formula
from the tap on macOS. Builds set `SOURCE_DATE_EPOCH`, map source paths with
`-ffile-prefix-map`, and pack archives with fixed ordering, ownership and
timestamps. Each archive's `BUILDINFO` records the commit, runner image,
compiler and LLVM version used.

## License

Apache License 2.0. See [LICENSE](LICENSE).
