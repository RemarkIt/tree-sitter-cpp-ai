# tree-sitter-cpp-ai

C/C++ grammar for [tree-sitter](https://github.com/tree-sitter/tree-sitter) that
parses real-world code containing unexpanded macros. It is an AI-developed fork
of [tree-sitter-cpp](https://github.com/tree-sitter/tree-sitter-cpp), based on
upstream commit `c009222`, and it remains a drop-in replacement: the language is
still `cpp` and the C entry point is still `tree_sitter_cpp`.

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

Held-out sample: 954 first-party files from ten public repositories (terminal,
notepad++, wil, leveldb, json, imgui, curl, qBittorrent, godot and folly),
pinned in [lab/corpora/heldout.json](lab/corpora/heldout.json). No grammar
change was developed against this sample.

| | upstream | this fork |
|---|---:|---:|
| Files with parse errors | 490 (51.4%) | 209 (21.9%) |
| ERROR nodes | 7,958 | 2,084 |
| Bytes inside ERROR regions | 1,133,588 | 480,620 |
| Error-free files that start failing | | 0 |

Speed on the same sample, relative to upstream (the lab's budgets are ≤1.20×
for totals and ≤1.25× for p95 and incremental):

| total parse | parse + declaration projection | per-file p95 | incremental reparse |
|---:|---:|---:|---:|
| 1.15× | 1.07× | 1.10× | 1.03× |

## Usage

The packages are named `tree-sitter-cpp-ai` so they can be installed next to
upstream. For example, in Python:

```sh
pip install git+https://github.com/RemarkIt/tree-sitter-cpp-ai
```

```python
import tree_sitter, tree_sitter_cpp_ai
parser = tree_sitter.Parser(tree_sitter.Language(tree_sitter_cpp_ai.language()))
```

Node types are a superset of upstream's, and some upstream misparses now get
different, correct trees. Queries written for upstream may need to handle the
new recovery nodes.

## Laboratory

[lab/](lab/README.md) contains the tooling used to develop the grammar:
Clang-checked fixtures, a regression gate, corpus census and timing against
upstream, deterministic GLR parse-work checks, and investigation tools to
reduce, anonymize, diff and profile parses.

## License

MIT, as upstream. See [LICENSE](LICENSE).
