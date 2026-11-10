"""Evidence for bench correctness, including known failures of the retained patch."""

from pathlib import Path
import json
import subprocess
import sys

import pytest

import bench
import cpp_build as build

# Fixtures the control patch (rejected.patch) fixes; it fixes nothing else.
CONTROL_IMPROVEMENTS = {
    "annotation-definition", "annotation-alias", "annotation-pointer", "annotation-namespace",
    "annotation-method", "annotation-prototype", "annotation-alias-prototype",
    "annotation-pointer-prototype",
}


@pytest.fixture(scope="session")
def comparison():
    return bench.compare(build.DEFAULT_CACHE, build.DEFAULT_CACHE / "latest" / "compare.json")


@pytest.fixture(scope="session")
def native_result():
    return build.native(build.DEFAULT_CACHE, build.DEFAULT_CACHE / "latest" / "native.json")


def test_native_corpus_uses_the_fingerprinted_generated_sources(native_result):
    assert native_result, (build.DEFAULT_CACHE / "latest" / "native.json").read_text()


@pytest.mark.parametrize("case", bench.fixtures(), ids=lambda case: case["id"])
def test_compiler_checked_parser_verdicts(comparison, case):
    record = next(row for row in comparison["cases"] if row["id"] == case["id"])
    assert record["oracle"] == record["expected"], record
    if case["id"] in CONTROL_IMPROVEMENTS:
        assert record["change"] == "improved", record
    elif case["id"] == "conditional-undef":
        assert record["checks"]["baseline"]["passed"] is False, record
        assert record["checks"]["candidate"]["wrong_expressions"] == ["a * API_CALL * c;"], record
    elif case["id"] in {"product", "product-uppercase", "product-call"}:
        assert record["change"] == "regressed", record
        assert record["checks"]["candidate"]["unexpected"], record
        assert record["checks"]["candidate"]["wrong_expressions"] == case["expressions"], record
    elif case.get("known_gap"):
        assert record["checks"]["baseline"]["passed"] is False, record
    else:
        # The control patch has no effect beyond the cases above. Fixtures for
        # constructs upstream misparses fail in both lanes (unchanged_bad).
        assert record["change"] in {"unchanged_good", "unchanged_bad"}, record


def test_comparison_retains_all_three_regressions(comparison):
    assert comparison["summary"]["regressions"] == ["product", "product-uppercase", "product-call"]
    assert comparison["summary"]["oracle_checked"] == len(bench.fixtures())
    assert all(Path(row["evidence"], "clang-ast.json").is_file() for row in comparison["cases"])


def test_difference_preserves_duplicate_symbols():
    symbol = bench.Symbol("a.cpp", "f", "function", (), 0, 10, True, "int")
    assert bench.difference([symbol], [symbol, symbol])["unexpected"] == [bench.asdict(symbol)]


def test_expression_checks_preserve_declared_shape():
    lanes = build.builds(build.DEFAULT_CACHE)
    parser = bench.parser_for(Path(lanes["baseline"]["library"]))
    data = b"int f(int); int run(int a, int b) { f(a); a * b * f(a); return 0; }"
    root = parser.parse(data).root_node
    assert not root.has_error, str(root)
    assert bench.wrong_expressions(root, data, [
        {"source": "f(a);", "kind": "call_expression"}, "a * b * f(a);",
    ]) == [], str(root)
    assert bench.wrong_expressions(root, data, ["f(a);"]) == ["f(a);"], str(root)
    assert bench.wrong_expressions(root, data, [
        {"source": "a * b * f(a);", "kind": "call_expression"},
    ]) == ["a * b * f(a);"], str(root)


def test_raw_node_checks_require_exact_physical_fields():
    lanes = build.builds(build.DEFAULT_CACHE)
    parser = bench.parser_for(Path(lanes["baseline"]["library"]))
    data = b"int run() { send(1); }"
    root = parser.parse(data).root_node
    expectation = {"kind": "call_expression", "source": "send(1)", "fields": {"function": "send"}}

    assert bench.wrong_raw_nodes(root, data, [expectation]) == []
    assert bench.wrong_raw_nodes(root, data, [expectation | {"fields": {"function": "other"}}]) == ["send(1)"]
    assert bench.wrong_raw_nodes(root, data, [expectation | {"kind": "uncertain_macro_statement"}]) == ["send(1)"]
    with pytest.raises(build.BenchError, match="exactly once"):
        bench.wrong_raw_nodes(root, data, [expectation | {"source": "absent"}])


def test_expected_source_is_independent_and_unambiguous():
    case = {"id": "duplicate", "file": "a.cpp", "symbols": [{"name": "x", "source": "int x;"}]}
    with pytest.raises(build.BenchError, match="exactly once"):
        bench.expected_symbols(case, {"a.cpp": b"int x; int x;"})


def test_raw_population_separates_only_expansion_generated_declarations():
    source = {"name": "after", "source": "int after() { return 1; }"}
    generated = {"name": "generated", "origin": "macro-expansion", "source": "scenario(1) { return 0; }"}
    case = {"id": "raw", "file": "a.cpp", "symbols": [source, generated], "raw_symbols": [source]}
    sources = {"a.cpp": b"scenario(1) { return 0; }\nint after() { return 1; }"}
    expected = bench.expected_symbols(case, sources)

    assert bench.raw_symbol_expectation(case, sources, expected) == [expected[0]]
    assert bench.raw_symbol_expectation({"symbols": [source, generated]}, sources, expected) == expected
    for entries in ([], [source | {"name": "invented"}], [source, generated]):
        with pytest.raises(build.BenchError, match="retain all independently expected source"):
            bench.raw_symbol_expectation(case | {"raw_symbols": entries}, sources, expected)


def test_compiler_oracle_retains_file_elision_at_first_macro_declaration(tmp_path):
    data = b"make() {}\nint after() { return 1; }"
    ast = {"inner": [
        {"kind": "FunctionDecl", "name": "generated", "type": {"qualType": "int ()"},
         "loc": {"spellingLoc": {"file": "a.cpp", "offset": 99}, "expansionLoc": {"offset": 0}},
         "range": {"begin": {"expansionLoc": {"offset": 0}}, "end": {"offset": 8, "tokLen": 1}},
         "inner": [{"kind": "CompoundStmt"}]},
        {"kind": "FunctionDecl", "name": "after", "type": {"qualType": "int ()"}, "loc": {"offset": 14},
         "range": {"begin": {"offset": 10}, "end": {"offset": len(data) - 1, "tokLen": 1}},
         "inner": [{"kind": "CompoundStmt"}]},
    ]}
    case = {"id": "elision", "file": "a.cpp", "symbols": [
        {"name": "generated", "origin": "macro-expansion", "source": "make() {}"},
        {"name": "after", "source": "int after() { return 1; }"},
    ]}

    assert bench.difference(bench.expected_symbols(case, {"a.cpp": data}),
                            bench.clang_symbols(ast, {"a.cpp": data}, tmp_path)) == {
        "missing": [], "unexpected": [], "changed": [],
    }


def test_destructor_projection_uses_token_identity_without_intertoken_whitespace():
    lanes = build.builds(build.DEFAULT_CACHE)
    parser = bench.parser_for(Path(lanes["baseline"]["library"]))
    data = b"struct Device { ~ Device() {} };"
    root = parser.parse(data).root_node
    case = {"id": "destructor", "file": "a.cpp", "symbols": [
        {"name": "~Device", "owner": ["Device"], "return_type": None, "source": "~ Device() {}"},
    ]}

    assert not root.has_error, str(root)
    assert bench.tree_symbols("a.cpp", data, root) == bench.expected_symbols(case, {"a.cpp": data})


def test_projection_retains_reference_parenthesized_and_function_pointer_declarators():
    lanes = build.builds(build.DEFAULT_CACHE)
    parser = bench.parser_for(Path(lanes["baseline"]["library"]))
    data = (
        b"int value;\n"
        b"int &reference() { return value; }\n"
        b"int (/* name */ parenthesized)() { return value; }\n"
        b"int (*/* pointer */ callback)(int);\n"
        b"int (declared)(int);\n"
        b"int (__stdcall *convention)(int);\n"
    )
    case = {"id": "declarator-projection", "file": "a.cpp", "symbols": [
        {"name": "value", "kind": "variable", "source": "int value;"},
        {"name": "reference", "return_type": "int&", "source": "int &reference() { return value; }"},
        {"name": "parenthesized", "source": "int (/* name */ parenthesized)() { return value; }"},
        {"name": "callback", "kind": "variable", "source": "int (*/* pointer */ callback)(int);"},
        {"name": "declared", "definition": False, "source": "int (declared)(int);"},
        {"name": "convention", "kind": "variable", "source": "int (__stdcall *convention)(int);"},
    ]}
    root = parser.parse(data).root_node

    assert not root.has_error, str(root)
    assert bench.tree_symbols("a.cpp", data, root) == bench.expected_symbols(case, {"a.cpp": data})


def test_compiler_oracle_keeps_fields_and_lambda_locals_without_generated_closure_methods(tmp_path):
    data = b"struct Holder { int member; }; int f() { [] { int local = 1; }(); }"

    def source_node(kind, name, source, **extra):
        start = data.index(source)
        return {
            "kind": kind, "name": name, "loc": {"offset": start},
            "range": {"begin": {"offset": start}, "end": {"offset": start + len(source) - 1, "tokLen": 1}},
            **extra,
        }

    ast = {"inner": [
        {"kind": "CXXRecordDecl", "name": "Holder", "id": "holder",
         "loc": {"file": "a.cpp"}, "inner": [
             source_node("FieldDecl", "member", b"int member"),
         ]},
        source_node("FunctionDecl", "f", b"int f() { [] { int local = 1; }(); }",
                    type={"qualType": "int ()"}, inner=[
                        {"kind": "CompoundStmt", "inner": [
                            {"kind": "LambdaExpr", "inner": [
                                {"kind": "CXXRecordDecl", "isImplicit": True,
                                 "definitionData": {"isLambda": True}, "inner": [
                                     source_node("CXXMethodDecl", "operator()", b"[] { int local = 1; }",
                                                 type={"qualType": "int ()"}, inner=[{"kind": "CompoundStmt"}]),
                                 ]},
                                {"kind": "CompoundStmt", "inner": [
                                    source_node("VarDecl", "local", b"int local = 1"),
                                ]},
                            ]},
                        ]},
                    ]),
    ]}
    case = {"id": "lambda-field-oracle", "file": "a.cpp", "symbols": [
        {"name": "member", "kind": "variable", "owner": ["Holder"], "source": "int member;"},
        {"name": "f", "source": "int f() { [] { int local = 1; }(); }"},
        {"name": "local", "kind": "variable", "owner": ["f"], "source": "int local = 1;"},
    ]}

    assert bench.difference(bench.expected_symbols(case, {"a.cpp": data}),
                            bench.clang_symbols(ast, {"a.cpp": data}, tmp_path)) == {
        "missing": [], "unexpected": [], "changed": [],
    }


def test_missing_tool_retains_explicit_failure(tmp_path):
    log = tmp_path / "tool.log"
    with pytest.raises(build.BenchError, match="Cannot execute"):
        build.run([str(tmp_path / "absent-tool")], tmp_path, log)
    assert "absent-tool" in log.read_text()


def test_nonzero_tool_is_not_success(tmp_path):
    log = tmp_path / "tool.log"
    with pytest.raises(build.BenchError, match="exited 7"):
        build.run(["/bin/sh", "-c", "echo broken >&2; exit 7"], tmp_path, log)
    assert "broken" in log.read_text()


def test_missing_or_tampered_library_is_rejected(tmp_path):
    library = tmp_path / "cpp.dylib"
    library.write_bytes(b"original")
    build.write_json(tmp_path / "builds.json", {
        "baseline": {"library": str(library), "library_sha256": "incorrect"},
    })
    with pytest.raises(build.BenchError, match="changed or is missing"):
        build.builds(tmp_path)


def test_native_failure_does_not_hide_other_lane(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(build, "builds", lambda _cache: {
        "baseline": {"checkout": str(tmp_path / "baseline"), "cli": "controlled-cli", "library": "baseline.dylib"},
        "candidate": {"checkout": str(tmp_path / "candidate"), "cli": "controlled-cli", "library": "candidate.dylib"},
    })

    def execute(argv, cwd, log, timeout):
        calls.append(cwd.name)
        if cwd.name == "baseline":
            raise build.BenchError("known failure")
        return "198 passed"

    monkeypatch.setattr(build, "run", execute)
    output = tmp_path / "native.json"
    assert build.native(tmp_path, output) is False
    assert calls == ["baseline", "candidate"]
    assert json.loads(output.read_text())["lanes"]["candidate"]["passed"] is True


def test_cli_invalid_input_is_an_error_not_a_parser_verdict(tmp_path):
    result = subprocess.run(
        [sys.executable, str(build.LAB / "bench.py"),
         "compare", "--cache", str(tmp_path), "--output", str(tmp_path / "comparison.json")],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 2
    assert "run prepare first" in result.stderr
    assert not (tmp_path / "comparison.json").exists()


def test_cli_reports_candidate_mismatches_without_success_fallback(tmp_path, comparison):
    output = tmp_path / "comparison.json"
    result = subprocess.run(
        [sys.executable, str(build.LAB / "bench.py"),
         "compare", "--output", str(output)],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 1, result.stderr
    report = json.loads(output.read_text())
    assert report["summary"] == comparison["summary"]
    assert json.loads(result.stdout) == report["summary"]
