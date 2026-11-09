# Methodology

This fork improves how tree-sitter parses real-world C and C++, above all code
that depends on macros the parser cannot expand. These rules decide whether a
grammar change is accepted.

## Raw bytes only

The parser reads the original physical source bytes. It never runs a
preprocessor or compiler, resolves includes or compilation databases, rewrites
or normalizes source, or consults macro definitions. Compilers appear only in
the [laboratory](../lab/README.md), as offline oracles for test fixtures.

## Generic syntax, no guesses

Every mechanism must be justified purely by C/C++ syntax, so that it works on
any codebase:

- No macro-name lists, project or path rules, capitalization heuristics, or
  thresholds tuned to a corpus.
- A shape is recovered only when local syntax certifies it. For example, a
  semicolonless statement macro is accepted only where the external scanner has
  checked the token sequence that follows it. Where the evidence is ambiguous,
  the upstream reading is kept.
- Recovered regions are labeled. Macro-dependent readings appear as
  `uncertain_*` or `macro_*` nodes, and irregular `#if` groups as
  `preproc_inline_conditional` / `preproc_inactive_branch`, so consumers can
  tell certified syntax from recovery.
- Fewer errors obtained by swallowing syntax or inventing declarations is not
  an improvement. Declaration fidelity (names, ownership, ranges) matters as
  much as error counts.

Tree-sitter can insert an external token as a MISSING node during error
recovery even when the scanner declined it. A scanner refusal is therefore not
an admission gate: only nonempty tokens the scanner actually produced may
certify a recovery, and zero-width tokens must never decide between two valid
readings that share a parse state.

## Evidence for a change

1. **Reproduce on a fixture.** Reduce the failing sample to the smallest input
   that keeps the failure (`lab/tools.py reduce`), then anonymize it
   (`lab/tools.py anonymize`) or rewrite it by hand. Fixtures never contain
   code copied from private sources.
2. **Record expectations independently.** A native corpus case in
   `test/corpus/raw_*.txt` pins the intended tree; a Clang-checked case in
   `lab/fixtures/cases.json` pins declarations. Add ordinary controls next to
   every recovery shape, such as braced initializers, multiplication,
   function-pointer variables and constructors, that must keep their upstream
   reading.
3. **Fix generically**, then validate:
   - `cpp_dev.py native` and `cpp_dev.py gate` pass;
   - the census on development corpora shows no newly broken files, and every
     changed error-free tree and declaration delta is reviewed;
   - held-out corpora that were never used for development confirm the change
     generalizes;
   - timing and deterministic parse work stay within budget (medians ≤1.20×,
     p95 and incremental ≤1.25× of upstream).

## Private corpora

Measuring on private code is fine, and is often where failures are found. Keep
private sources, manifests, reports and file names out of this repository,
including fixtures, docs and commit messages. Reproduce private failures with
reduced, anonymized or hand-written fixtures.

## Known limits

- Tree-sitter keeps at most six GLR versions. Deeply nested qualified template
  ambiguities can exceed that and fail in upstream as well.
- Code whose syntax only exists after macro expansion, such as a macro that
  expands to a declarator head, cannot be parsed faithfully from raw bytes.
- An irregular `#if` group, one whose branches do not each form complete
  syntax, is parsed in place through its last branch; earlier branches stay
  opaque `preproc_inactive_branch` nodes.
