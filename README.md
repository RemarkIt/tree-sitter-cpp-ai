# tree-sitter-cpp-ai

C/C++ grammar for [tree-sitter](https://github.com/tree-sitter/tree-sitter) that
parses real-world code containing unexpanded macros. It is an AI-developed fork
of [tree-sitter-cpp](https://github.com/tree-sitter/tree-sitter-cpp), based on
upstream commit `c009222` with upstream's full history preserved, and it remains
a drop-in replacement: the language is still `cpp` and the C entry point is
still `tree_sitter_cpp`.

Tree-sitter grammars see source before preprocessing, so code like this:

```cpp
class EXPORT_API Widget : public Base {
public:
    int size() const;
};

DECLARE_HANDLER(OnClick)

int API_CALL compute(int x) {
    LOG_SCOPE(x)
    return x;
}
```

breaks upstream: `Widget` becomes a function definition containing an ERROR
node, and `compute` loses statements to missing semicolons. This fork parses
the class `Widget`, the `DECLARE_HANDLER` invocation and the function `compute`
with its `LOG_SCOPE` statement. It does this from local syntax alone, with no
preprocessor, compiler, macro lists or naming guesses, and labels every
recovered region (`uncertain_*`, `macro_*`, `preproc_inline_conditional`) so
consumers can tell certified syntax from recovery. See the
[methodology](docs/methodology.md).

## Results

Measured once, for release 0.1.0, on a fresh sample whose file selection was
fixed before its first parse: 1,300 first-party files, up to 100 per
repository, from 13 public repositories never used to develop the grammar
(bitcoin, opencv, protobuf, abseil-cpp, googletest, duckdb, obs-studio, CMake,
calculator, serenity, redis, llama.cpp and Marlin), pinned in
[lab/corpora/fresh.json](lab/corpora/fresh.json).

| | upstream | this fork |
|---|---:|---:|
| Files with parse errors | 475 (36.5%) | 111 (8.5%) |
| ERROR nodes | 4,920 | 784 |
| MISSING nodes | 2,128 | 148 |
| Bytes inside ERROR regions | 675,192 | 27,131 |
| Error-free files that start failing | | 0 |

Every repository improves; the files still containing errors range from 2
(duckdb) to 20 (googletest) per 100. No function disappears from a file that
upstream parses without errors. The 237 function spans that do disappear all
come from files upstream already misparsed, where text such as
`ABSL_NAMESPACE_BEGIN namespace x {` or `class X CV_FINAL : public Base {` was
read as a function definition; the real functions and constructors appear in
their place (320 spans).

That measurement was blind. Its failures then exposed standard syntax that
neither upstream nor the fork parsed: GNU attributes after declarators, unnamed
bit-fields, GNU case ranges, pointer-to-member typedefs, `new const T`,
`delete[] (T*)p`, `long{1}`, `T __restrict p` and constrained template
parameters with type defaults. With those fixed, the same sample has 99 files
with errors (7.6%), 753 ERROR and 137 MISSING nodes and 26,921 ERROR-region
bytes, with no file newly broken and no error-free tree changed. Because these
failures guided the fixes, the sample now also counts as development code.

On the code the grammar was developed against, the gap is wider:
[lab/corpora/heldout.json](lab/corpora/heldout.json), 954 files from ten other
repositories that were held out at first and later used for development, goes
from 490 to 12 files with errors. Expect results on new code to resemble the
fresh sample rather than this one.

Parse time on the fresh sample relative to upstream, as per-thread CPU time
with both parsers compiled at `-O2`:

| clean in both (825 files) | fixed by this fork (364) | errors in both (111) | all (1,300) |
|---:|---:|---:|---:|
| 1.06× | 0.90× | 0.96× | 0.98× |

Per-file p95 is 1.03× and incremental reparsing 0.74× (the lab's budgets are
≤1.20× for totals and ≤1.25× for p95 and incremental). Code that already parses
cleanly pays about 6% for the extra checks; code the fork repairs is faster
because the parser no longer runs error recovery.

## Trade-offs and limits

- **Size:** the generated `src/parser.c` is about 71 MB (upstream 26 MB) and the
  compiled library about 17 MB (upstream 5.6 MB), because certified macro shapes
  need about 30,000 parse states (upstream about 11,700).
- **Memory:** holding parsed trees takes about 18% more than upstream, because
  tree-sitter stores tokens from an external scanner as full nodes.
- **Trees:** node types are a superset of upstream's, and some upstream
  misparses get different, correct trees, so queries written for upstream may
  need to handle the new nodes.
- **Remaining errors:** template-heavy library headers, test frameworks whose
  macros take arguments that are not expressions, and code whose syntax only
  exists after macro expansion. See the [methodology's known limits](docs/methodology.md#known-limits).
- **Status:** 0.x and experimental; the tree shape may still change between
  minor versions.

## Usage

The packages are named `tree-sitter-cpp-ai` so they can be installed next to
upstream. For example, in Python:

```sh
pip install tree-sitter-cpp-ai
```

```python
import tree_sitter, tree_sitter_cpp_ai
parser = tree_sitter.Parser(tree_sitter.Language(tree_sitter_cpp_ai.language()))
```

## Laboratory

[lab/](lab/README.md) contains the tooling used to develop the grammar:
Clang-checked fixtures, a regression gate, corpus census and timing against
upstream, deterministic GLR parse-work checks, and investigation tools to
reduce, anonymize, diff and profile parses.

## Source layout

`grammar.js` extends upstream's grammar; each external token's contract is
documented at its declaration in `externals`. The external scanner is
`src/scanner.c`, which lists every token beside the function that decides it
and includes its modules from `src/scanner/`: `lexing.h` (characters, words,
trivia, literals), `words.h` (keyword lists), `balanced.h` (bracket, block and
argument scans), `conditionals.h` (preprocessor conditionals),
`declarations.h` (annotation and macro certification) and `raw_strings.h`.

## License

MIT, as upstream. See [LICENSE](LICENSE).
