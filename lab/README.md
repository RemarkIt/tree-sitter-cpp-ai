# C/C++ parser laboratory

Measurement and investigation tooling for this grammar. The lab compares a
candidate parser against pinned upstream
[tree-sitter-cpp](https://github.com/tree-sitter/tree-sitter-cpp) at
`c009222808634c1014f82438d4883753516a2c24`, using Clang only as an offline test
oracle. Parsing itself never runs a compiler or preprocessor. See the
[methodology](../docs/methodology.md) for the rules a grammar change must satisfy.

## Setup

Requirements: macOS (the native build recipe uses `cc -dynamiclib`), Git,
Node/npm, Python 3.10+, and Clang with the `i686-pc-windows-msvc` frontend
target (stock Apple/LLVM Clang works).

```sh
python3 -m venv .venv && .venv/bin/pip install -r lab/requirements.txt
.venv/bin/python lab/bench.py prepare --upstream-source .   # upstream baseline + control candidate
.venv/bin/python lab/cpp_dev.py init                         # generation workspace and npm dependencies
```

Everything the lab writes lives under the ignored cache `.cache/lab/`, or under
`$CPP_LAB_CACHE` when set. `--upstream-source .` clones the pinned upstream
commit from this repository's own history instead of the network.
`prepare --cli /path/to/tree-sitter` reuses an installed tree-sitter 0.26.5
binary, verified by version and hash, instead of npm's binary download.

## Development loop

```sh
.venv/bin/python lab/cpp_dev.py build        # generate + compile the working tree's grammar
.venv/bin/python lab/cpp_dev.py native       # tree-sitter corpus tests of that build
.venv/bin/python lab/cpp_dev.py gate         # Clang-checked fixture regression gate
.venv/bin/python lab/cpp_dev.py measure --case <id>   # focused fixture measurement
.venv/bin/python lab/cpp_dev.py generated --write     # refresh committed src/ outputs
```

`build` copies the handwritten generation inputs (`grammar.js`,
`src/scanner.c` and its `src/scanner/*.h` modules, package manifests and lock,
`tree-sitter.json`, queries and tests) into an owned workspace, so generation never writes into the checkout.
Unchanged inputs reuse their library; each input identity gets a distinct
library path. A new top-level `*.js` module or `src/*.[ch]` file fails until it
is added to the recipe in `cpp_dev.py`. `generated` exits 1 when the committed
`src/parser.c`, `src/grammar.json`, `src/node-types.json` or `src/tree_sitter/`
headers differ from the pinned CLI's output for the current inputs.

Evidence tracks stay separate:

- `gate` protects every fixture the upstream baseline parses correctly plus
  fixtures marked `required`; it cannot be filtered. Known gaps stay visible
  without failing it.
- `measure` reports every unresolved case and exits 0 when measurement
  completes; it is not acceptance.
- `bench.py compare` measures the prepared control candidate,
  [rejected.patch](rejected.patch): an early prototype that made multiplication
  parse as declarations. Lab tests assert the harness detects its defects.

Exit codes: `0` success, `1` candidate mismatch or regression, `2` invalid
input, compiler-oracle disagreement or tooling failure.

### Round check

```sh
.venv/bin/python lab/round.py check --name r34 --previous r33 [--timing]
.venv/bin/python lab/round.py timing --name r34   # time the pinned build separately, update the verdict
```

One command per grammar change: build and refresh the committed generated
sources (as `generated --write` does, because lab tests read them), then native
tests followed by lab tests in the background while the gate and a census of
every configured corpus run against the previous round's library, then optional
timing against upstream. The build is pinned as
`.cache/lab/rounds/<name>.dylib`, so the next round can name it with
`--previous`. Corpora come from `.cache/lab/rounds/config.json`
(`{"manifests": {"heldout": "/path/manifest.json"}, "timing": "heldout"}`) or
repeated `--manifest NAME=PATH`, so private corpus paths never enter the
repository. It prints about ten lines:

- per corpus: error files, sites and bytes before → after, newly broken and
  newly clean files, clean files whose tree changed, and declarations lost or
  gained against the previous round. A declaration that moved or changed kind
  is not lost; names that only appear when macros, keywords, annotations or
  member initializers are misread count as `junk lost`.
- the gate count, regressions and cases newly passing or failing against the
  previous round's summary.

Exit codes: `0` accept (all checks pass and some corpus count fell), `1` fail
(build, native, lab tests, gate, timing or a newly broken file), `2` review
(clean files changed, declarations lost, a count rose, or nothing improved),
`3` the check could not run. Full evidence, including `summary.json`, the
census reports and logs, stays in `.cache/lab/rounds/<name>/`; `tools.py
reduce` and `diff` with the two pinned libraries turn a listed file into a
reproducer.

## Fixtures

[Fixtures](fixtures/) are self-contained, hand-written C/C++ sources. The
[case manifest](fixtures/cases.json) supplies the expected declarations
independently: name, kind, owner, return type and the exact source snippet,
which must occur exactly once. Clang (`--target=i686-pc-windows-msvc
-fms-extensions -nostdinc`, C++20 unless a case says otherwise) must agree with
the expectations before parser results count. Case fields:

- `symbols`: the complete compiler-checked function and variable population.
- `raw_symbols`: optional subset excluding expansion-only declarations; it must
  keep every source-written expectation.
- `raw_nodes`: exact node kinds, spans and named fields.
- `expressions`: statements that must keep their expression shape.
- `forbid_uncertainty`: ordinary controls where no `uncertain_*`/`macro_*`
  recovery node may appear.
- `known_gap` / `required`: unresolved measurement versus mandatory gate case.
- `headers`, `language`, `standard`, `compiler_flags`: compilation context.

Native corpus tests live in [test/corpus](../test/corpus/). The
`raw_*.txt` files pair each recovery shape with ordinary controls that must keep
their upstream reading.

### Writing fixtures and lab tests

- Group fixtures by failure family. Pair every positive case with ordinary
  controls: identifiers and expressions, macro redefinition, includes, C, and
  macro-like text in literals and comments. Keep braced initialization,
  qualified constructors/destructors and neighboring method scopes as recovery
  controls.
- Assert an expression's node kind as well as its source; call controls must
  not inherit the binary-expression default.
- Constructor and destructor expectations have a null return type; name
  identity ignores legal whitespace between tokens.
- The Clang oracle includes named fields but not generated lambda closure
  members; locals in lambda bodies stay in the population. Preserve Clang's
  file elision at macro spelling and expansion locations without substituting
  spelling offsets for physical ranges.
- The projection follows reference and parenthesized declarators even without
  a named `declarator` field, skipping comments and calling-convention modifiers.
- Keep macro and uncertainty regions separate from syntax errors and from
  explicit projection gaps.
- Test ownership: `test_bench.py` covers oracle agreement and the control
  candidate's known defects (they certify measurement, not acceptance);
  `test_dev.py` covers input identity, non-destructive setup, workspace
  synchronization, cache and library integrity, and returning to cached inputs
  restoring both generated sources and the selected receipt; `test_census.py`
  and `test_syntax_census.py` cover manifest hashes and fixed denominators,
  declaration multiplicity and range deltas, timing aggregation with controlled
  clocks, parse-work budgets and incremental/fresh agreement; `test_tools.py`
  covers the investigation tools; `test_round.py` covers the round verdict and
  declaration matching; `test_scanner_words.py` keeps the scanner's
  word lists sorted for binary search and its `KEYWORDS` equal to the grammar's
  identifier-shaped literals, because the scanner returns every other word as an
  identifier. Acceptance timing uses real native parsing and excludes compiler,
  setup and I/O costs.

## Corpora, census and timing

```sh
.venv/bin/python lab/corpus.py lab/corpora/heldout.json      # fetch pinned public files
.venv/bin/python lab/census.py --manifest .cache/lab/corpora/heldout/manifest.json \
  --candidate-library dev --output .cache/lab/heldout.json
.venv/bin/python lab/census.py ... --timing                  # isolated timing mode
```

A corpus specification pins repositories by commit and files by path and
SHA-256; sources are fetched on demand, never redistributed.
[fresh.json](corpora/fresh.json) is the sample behind the published blind
results; it and [heldout.json](corpora/heldout.json) have since guided
development, so a new generalization measurement needs a new sample. The census reports
error-containing files, ERROR and missing-node spans (including hidden missing
tokens), function-definition spans, declaration deltas with ownership and
ranges, uncertain regions and tree identities against upstream and an optional
`--previous-library`. A lower error count is not proof of declaration recall.

Timing preloads verified bytes and alternates baseline and candidate samples,
separating parse from parse-plus-projection, plus incremental edits checked
against fresh parses. It measures per-thread CPU time, which other processes
barely disturb, and libraries are compiled with `-O2` like packaged builds.
Budgets: medians ≤1.20×, per-file p95 and incremental ≤1.25×. Timing also
counts deterministic parse work (tree-sitter `process version` log events) on
ordinary inputs, each ≤1.25× baseline, which catches GLR alternatives that never
merge without clock noise. Tree-sitter merges versions only when their external
scanner states are byte-equal, so the scanner must serialize its default state
as zero bytes. An idle machine still gives the most stable cache behaviour.

Private corpora use the same manifest format (`files` entries with `path` and
`current_sha256` relative to a sibling `source/` directory). Keep private
sources, manifests and reports outside this repository.

## Investigation tools

`tools.py` accepts parser library paths or the aliases `upstream` and `dev`:

| Command | Purpose |
|---|---|
| `tree LIB FILE [--lines A:B]` | Parse tree with fields and 1-based lines |
| `diff OLD NEW FILE...` | Where two parsers' trees differ |
| `compare OLD NEW --manifest M` | Tree identity, fixed/broken files, moved first errors, declaration deltas |
| `reduce LIB FILE [--against LIB] [--property error\|differs]` | Delta-debug to the lines that keep a property |
| `anonymize FILE [--keep NAME] [--library LIB]` | Rename identifiers, blank literals and comments, keep syntax |
| `glr FILE LIB...` | GLR activity: processed versions and widest row |
| `time OLD NEW --manifest M` | Rank files by parse-time ratio |

## Limits

The projection covers functions and variables (names, ownership, return types,
spans), not types, references, callees or overloads. The Clang oracle covers
one explicit configuration; inactive preprocessor branches are outside its
evidence. C fixtures exercise C compatibility of this grammar, not a separate C
grammar.
