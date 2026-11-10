"""Compiler-checked fixture evidence for raw C/C++ parse trees."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import re
import sys
from dataclasses import asdict, dataclass
from importlib.metadata import version
from pathlib import Path
from typing import Any, Iterator

from tree_sitter import Language, Node, Parser

import cpp_build as build

LIBRARIES: list[ctypes.CDLL] = []
CLANG_FLAGS = ["--target=i686-pc-windows-msvc", "-fms-extensions", "-nostdinc"]


@dataclass(frozen=True)
class Symbol:
    path: str
    name: str
    kind: str
    owner: tuple[str, ...]
    start: int
    end: int
    definition: bool
    return_type: str | None
    origin: str = "source"


def normalized_type(text: str) -> str:
    return re.sub(r"\s+", "", text)


def parser_for(library: Path) -> Parser:
    if version("tree-sitter") != "0.25.2":
        raise build.BenchError("This bench requires tree-sitter==0.25.2; install requirements.txt.")
    handle = ctypes.CDLL(str(library.resolve(strict=True)))
    factory = handle.tree_sitter_cpp
    factory.argtypes = []
    factory.restype = ctypes.c_void_p
    pointer = factory()
    if pointer is None:
        raise build.BenchError(f"Null tree_sitter_cpp factory: {library}")
    capsule = ctypes.pythonapi.PyCapsule_New
    capsule.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
    capsule.restype = ctypes.py_object
    LIBRARIES.append(handle)
    return Parser(Language(capsule(pointer, b"tree_sitter.Language", None)))


def walk(node: Node) -> Iterator[Node]:
    yield node
    for child in node.named_children:
        yield from walk(child)


def is_uncertain(node: Node) -> bool:
    """Recovery nodes whose reading depends on unexpanded macros or a chosen conditional branch."""
    return node.type.startswith(("macro_", "uncertain_")) or node.type in {
        "preproc_inline_conditional", "preproc_inactive_branch",
    }


def text(node: Node, data: bytes) -> str:
    return data[node.start_byte:node.end_byte].decode("utf-8")


def named_declarator(node: Node) -> Node | None:
    if node.type in {"identifier", "field_identifier", "qualified_identifier", "operator_name", "destructor_name"}:
        return node
    children = node.children_by_field_name("declarator")
    if not children and node.type in {"reference_declarator", "parenthesized_declarator"}:
        children = [child for child in node.named_children if child.type not in {"comment", "ms_call_modifier"}]
    for child in children:
        found = named_declarator(child)
        if found is not None:
            return found
    return None


def declares_function(function: Node) -> bool:
    """A function declarator declares a function unless its name is behind indirection."""
    inner = function.child_by_field_name("declarator")
    while inner is not None and inner.type in {"parenthesized_declarator", "annotated_declarator", "attributed_declarator"}:
        inner = inner.child_by_field_name("declarator") or next((
            child for child in inner.named_children
            if child.type not in {"comment", "ms_call_modifier", "attribute_declaration"}
        ), None)
    return inner is not None and inner.type not in {"pointer_declarator", "reference_declarator", "array_declarator"}


def owners(node: Node, data: bytes) -> tuple[str, ...]:
    result = []
    parent = node.parent
    while parent is not None:
        if parent.type in {"namespace_definition", "class_specifier", "struct_specifier", "function_definition"}:
            name = parent.child_by_field_name("name")
            if parent.type == "function_definition":
                declarator = parent.child_by_field_name("declarator")
                name = named_declarator(declarator) if declarator else None
            if name:
                result.append(text(name, data))
        parent = parent.parent
    return tuple(reversed(result))


def tree_symbols(path: str, data: bytes, root: Node) -> list[Symbol]:
    result = []
    for node in walk(root):
        if node.type not in {"function_definition", "declaration", "field_declaration"}:
            continue
        for declarator in node.children_by_field_name("declarator"):
            name_node = named_declarator(declarator)
            if name_node is None:
                continue
            qualified = [part.strip() for part in text(name_node, data).split("::")]
            if qualified[-1].startswith("~"):
                qualified[-1] = "~" + qualified[-1][1:].lstrip()
            owner = owners(node, data)
            qualifiers = tuple(qualified[:-1])
            if qualifiers:
                owner = qualifiers if qualifiers[:len(owner)] == owner else owner + qualifiers
            functions = [child for child in walk(declarator) if child.type == "function_declarator"]
            function = next((child for child in functions if declares_function(child)), None)
            kind = "function" if node.type == "function_definition" or function else "variable"
            type_node = node.child_by_field_name("type")
            return_type = text(type_node, data) if kind == "function" and type_node else None
            if return_type is not None and function is not None:
                parent = function.parent
                while parent is not None and parent.id != node.id:
                    if parent.type == "pointer_declarator":
                        return_type += "*"
                    elif parent.type == "reference_declarator":
                        return_type += "&"
                    parent = parent.parent
            result.append(Symbol(
                path, qualified[-1], kind, owner, node.start_byte, node.end_byte,
                node.type == "function_definition",
                normalized_type(return_type) if return_type is not None else None,
            ))
    return result


def location(value: dict[str, Any]) -> dict[str, Any]:
    return value.get("expansionLoc", value)


def clang_symbols(ast: dict[str, Any], sources: dict[str, bytes], directory: Path) -> list[Symbol]:
    result: list[Symbol] = []
    contexts: dict[str, tuple[str, ...]] = {}
    current_file: Path | None = None

    def visit(node: dict[str, Any], owner: tuple[str, ...]) -> None:
        nonlocal current_file
        loc = location(node.get("loc", {}))
        # Clang elides repeated file names even across spelling/expansion locations.
        # Keep expansion offsets, but recover the file emitted on the spelling site.
        source_file = (
            loc.get("file") or location(node.get("range", {}).get("begin", {})).get("file")
            or node.get("loc", {}).get("spellingLoc", {}).get("file")
        )
        if source_file:
            path = Path(source_file)
            current_file = (path if path.is_absolute() else directory / path).resolve()
        file = current_file
        context = contexts.get(node.get("parentDeclContextId", ""), owner)
        implicit = node.get("isImplicit", False)
        kind = node.get("kind")
        # The lambda body is visited separately; its closure methods are generated.
        if kind in {"CXXRecordDecl", "RecordDecl"} and node.get("definitionData", {}).get("isLambda"):
            return
        if kind in {"NamespaceDecl", "CXXRecordDecl", "RecordDecl"} and node.get("name") and not implicit:
            context += (node["name"],)
            contexts[node["id"]] = context
        symbol_kind = "function" if kind in {
            "FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl",
        } else "variable" if kind in {"VarDecl", "FieldDecl"} else None
        if symbol_kind and not implicit and file is not None and file.is_relative_to(directory):
            relative = file.relative_to(directory).as_posix()
            if relative in sources:
                begin = location(node["range"]["begin"])
                end = location(node["range"]["end"])
                if "offset" not in begin or "offset" not in end:
                    raise build.BenchError(f"Compiler omitted a source range: {relative}:{node.get('name')}")
                start = begin["offset"]
                finish = end["offset"] + end["tokLen"]
                # `= default;` and `= delete;` are function bodies, as in the standard and the parser.
                explicit_body = symbol_kind == "function" and (
                    node.get("explicitlyDeleted", False) or "explicitlyDefaulted" in node
                )
                definition = explicit_body or symbol_kind == "function" and any(
                    child["kind"] == "CompoundStmt" for child in node.get("inner", [])
                )
                data = sources[relative]
                if explicit_body:
                    # Clang ends member extents after `= delete` but namespace-scope ones before it.
                    clause = re.match(rb"\s*=\s*(?:delete|default)\b", data[finish:])
                    if clause:
                        finish += clause.end()
                if not definition or explicit_body:
                    semicolon = finish
                    while semicolon < len(data) and data[semicolon:semicolon + 1] in b" \t\r\n":
                        semicolon += 1
                    if data[semicolon:semicolon + 1] == b";":
                        finish = semicolon + 1
                return_type = None
                if symbol_kind == "function" and kind not in {"CXXConstructorDecl", "CXXDestructorDecl"}:
                    return_type = normalized_type(node["type"]["qualType"].split("(", 1)[0])
                result.append(Symbol(
                    relative, node["name"], symbol_kind, context, start, finish, definition, return_type,
                    "macro-expansion" if "expansionLoc" in node.get("loc", {}) else "source",
                ))
        child_owner = context
        if symbol_kind == "function" and node.get("name"):
            child_owner += (node["name"],)
        for child in node.get("inner", []):
            visit(child, child_owner)

    visit(ast, ())
    return result


def expected_symbols(case: dict[str, Any], sources: dict[str, bytes]) -> list[Symbol]:
    result = []
    for entry in case["symbols"]:
        path = entry.get("path", case["file"])
        snippet = entry["source"].encode("utf-8")
        data = sources[path]
        if data.count(snippet) != 1:
            raise build.BenchError(f"{case['id']}: expected source must occur exactly once: {entry['source']}")
        start = data.index(snippet)
        result.append(Symbol(
            path, entry["name"], entry.get("kind", "function"), tuple(entry.get("owner", [])),
            start, start + len(snippet), entry.get("definition", entry.get("kind", "function") == "function"),
            normalized_type(entry.get("return_type", "int"))
            if entry.get("kind", "function") == "function" and entry.get("return_type", "int") is not None else None,
            entry.get("origin", "source"),
        ))
    return result


def raw_symbol_expectation(
    case: dict[str, Any], sources: dict[str, bytes], expected: list[Symbol],
) -> list[Symbol]:
    if "raw_symbols" not in case:
        return expected
    raw = expected_symbols(case | {"symbols": case["raw_symbols"]}, sources)
    delta = difference([symbol for symbol in expected if symbol.origin == "source"], raw)
    if delta["missing"] or delta["unexpected"]:
        raise build.BenchError(f"{case['id']}: raw_symbols must retain all independently expected source declarations")
    return raw


def difference(expected: list[Symbol], actual: list[Symbol]) -> dict[str, Any]:
    # Keep multiplicity: accidental duplicate declarations are evidence, not a set.
    remaining = list(actual)
    missing = []
    for symbol in expected:
        if symbol in remaining:
            remaining.remove(symbol)
        else:
            missing.append(asdict(symbol))
    unexpected = [asdict(symbol) for symbol in remaining]
    changed = []
    for previous in missing:
        identity = ("path", "name", "kind", "owner", "definition")
        matches = [
            current for current in unexpected
            if all(current[key] == previous[key] for key in identity)
        ]
        if len(matches) == 1:
            changed.append({
                "expected": previous, "actual": matches[0],
                "fields": [key for key in ("start", "end", "return_type", "origin") if previous[key] != matches[0][key]],
            })
    return {"missing": missing, "unexpected": unexpected, "changed": changed}


def fixtures() -> list[dict[str, Any]]:
    manifest = json.loads((build.LAB / "fixtures" / "cases.json").read_text(encoding="utf-8"))
    cases = manifest["cases"]
    ids = [case["id"] for case in cases]
    if len(set(ids)) != len(ids):
        raise build.BenchError("Duplicate fixture case IDs.")
    return cases


def case_sources(case: dict[str, Any]) -> dict[str, bytes]:
    root = build.LAB / "fixtures"
    sources = {}
    for relative in [case["file"], *case.get("headers", [])]:
        path = root / relative
        if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root.resolve()):
            raise build.BenchError(f"Unsafe fixture path: {relative}")
        sources[relative] = path.read_bytes()
    return sources


def wrong_expressions(root: Node, data: bytes, expected: list[str | dict[str, str]]) -> list[str]:
    failures = []
    for expression in expected:
        statement = expression if isinstance(expression, str) else expression["source"]
        kind = "binary_expression" if isinstance(expression, str) else expression["kind"]
        matches = [
            node for node in walk(root)
            if node.type == "expression_statement"
            and text(node, data) == statement
            and node.named_children and node.named_children[0].type == kind
        ]
        if len(matches) != 1:
            failures.append(statement)
    return failures


def wrong_raw_nodes(root: Node, data: bytes, expectations: list[dict[str, Any]]) -> list[str]:
    """Check physical node/field spans independently of compiler-expanded symbols."""
    failures = []
    nodes = list(walk(root))
    for entry in expectations:
        snippet = entry["source"].encode("utf-8")
        if data.count(snippet) != 1:
            raise build.BenchError(f"Raw node source must occur exactly once: {entry['source']}")
        start = data.index(snippet)
        matches = [
            node for node in nodes if node.type == entry["kind"]
            and (node.start_byte, node.end_byte) == (start, start + len(snippet))
        ]
        if len(matches) != 1 or any(
            (field_node := matches[0].child_by_field_name(field)) is None
            or text(field_node, data) != expected
            for field, expected in entry.get("fields", {}).items()
        ):
            failures.append(entry["source"])
    return failures


def compare(
    cache: Path, output: Path, candidate_library: Path | None = None, case_ids: list[str] | None = None,
) -> dict[str, Any]:
    cases = fixtures()
    if case_ids is not None:
        unknown = set(case_ids) - {case["id"] for case in cases}
        if not case_ids or unknown or len(set(case_ids)) != len(case_ids):
            raise build.BenchError(f"Case selection must be nonempty, unique and known; unknown: {sorted(unknown)}")
        cases = [case for case in cases if case["id"] in case_ids]
    lanes = build.builds(cache)
    selected = {
        "baseline": Path(lanes["baseline"]["library"]),
        "candidate": candidate_library.resolve(strict=True) if candidate_library else Path(lanes["candidate"]["library"]),
    }
    parsers = {name: parser_for(library) for name, library in selected.items()}
    output.parent.mkdir(parents=True, exist_ok=True)
    compiler = build.run(["clang++", "--version"], build.ROOT, output.parent / "clang-version.log")
    records = []
    for case in cases:
        sources = case_sources(case)
        expected = expected_symbols(case, sources)
        raw_expected = raw_symbol_expectation(case, sources, expected)
        directory = output.parent / "fixtures" / case["id"]
        directory.mkdir(parents=True, exist_ok=True)
        for relative, data in sources.items():
            destination = directory / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        flags = [*CLANG_FLAGS, *case.get("compiler_flags", [])]
        command = ["clang++", *flags, "-x", case.get("language", "c++"),
                   "-std=" + case.get("standard", "c++20"), "-fsyntax-only",
                   "-Wno-unused-value", "-Xclang", "-ast-dump=json", case["file"]]
        raw = build.run(command, directory, directory / "clang.log")
        ast = json.loads(raw)
        build.write_json(directory / "clang-ast.json", ast)
        oracle = clang_symbols(ast, sources, directory.resolve())
        oracle_diff = difference(expected, oracle)
        if oracle_diff["missing"] or oracle_diff["unexpected"]:
            build.write_json(directory / "oracle-difference.json", oracle_diff)
            raise build.BenchError(f"Invalid compiler oracle/expectation for {case['id']}; see {directory}")
        macro_command = ["clang++", *flags, "-x", case.get("language", "c++"),
                         "-std=" + case.get("standard", "c++20"), "-E", "-dM", case["file"]]
        build.run(macro_command, directory, directory / "macros.log")
        checks = {}
        for name, parser in parsers.items():
            symbols = []
            errors = []
            expression_errors = []
            raw_node_errors = []
            uncertainties: list[dict[str, Any]] = []
            trees = {}
            for relative, data in sources.items():
                root = parser.parse(data).root_node
                trees[relative] = str(root)
                symbols.extend(tree_symbols(relative, data, root))
                if root.has_error:
                    errors.append(relative)
                if relative == case["file"]:
                    expression_errors.extend(wrong_expressions(root, data, case.get("expressions", [])))
                    raw_node_errors.extend(wrong_raw_nodes(root, data, case.get("raw_nodes", [])))
                    uncertainties.extend(
                        {"kind": node.type, "start": node.start_byte, "end": node.end_byte}
                        for node in walk(root) if is_uncertain(node)
                    )
            delta = difference(raw_expected, symbols)
            passed = not (
                delta["missing"] or delta["unexpected"] or errors or expression_errors
                or raw_node_errors or (case.get("forbid_uncertainty", False) and uncertainties)
            )
            checks[name] = delta | {
                "passed": passed, "syntax_error_files": errors, "wrong_expressions": expression_errors,
                "wrong_raw_nodes": raw_node_errors, "uncertainties": uncertainties,
                "symbols": [asdict(symbol) for symbol in symbols],
            }
            build.write_json(directory / f"{name}-trees.json", trees)
        before, after = checks["baseline"]["passed"], checks["candidate"]["passed"]
        change = "unchanged_good" if before and after else "regressed" if before else "improved" if after else "unchanged_bad"
        records.append({
            "id": case["id"], "change": change, "checks": checks,
            "family": case.get("family", "unclassified"),
            "expected": [asdict(symbol) for symbol in expected], "oracle": [asdict(symbol) for symbol in oracle],
            "raw_expected": [asdict(symbol) for symbol in raw_expected],
            "raw_unavailable": [asdict(symbol) for symbol in expected if symbol not in raw_expected],
            "source_sha256": {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()},
            "compiler_argv": command, "evidence": str(directory),
        })
    report = {
        "schema": 1, "runtime": version("tree-sitter"), "compiler": compiler,
        "builds": {
            "baseline": lanes["baseline"],
            "candidate": lanes["candidate"] if candidate_library is None else {"origin": "external"},
        },
        "candidate_mode": "raw",
        "harness_sha256": {name: build.digest(build.LAB / name) for name in ("bench.py", "cpp_build.py")},
        "libraries": {name: {"path": str(path), "sha256": build.digest(path)} for name, path in selected.items()},
        "fixture_manifest_sha256": build.digest(build.LAB / "fixtures" / "cases.json"),
        "cases": records,
        "summary": {
            "cases": len(records), "oracle_checked": len(records),
            "baseline_passed": sum(row["checks"]["baseline"]["passed"] for row in records),
            "candidate_passed": sum(row["checks"]["candidate"]["passed"] for row in records),
            "regressions": [row["id"] for row in records if row["change"] == "regressed"],
            "improvements": [row["id"] for row in records if row["change"] == "improved"],
        },
        "limitations": [
            "Experimental function/variable projection, not complete C++ extraction or symbol recall.",
            "Clang is a test oracle for one explicit target/configuration; inactive branches are not compiler evidence.",
            "Macro dumps describe end-of-translation-unit state, not line-local definitions.",
            "C fixtures use the C++ grammar for compatibility evidence, not a separately built C grammar.",
        ],
    }
    build.write_json(output, report)
    return report


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("prepare", help="Build pinned upstream and a selected patch once.")
    setup.add_argument("--upstream-source", default=build.UPSTREAM)
    setup.add_argument("--patch", type=Path, default=build.LAB / "rejected.patch")
    setup.add_argument("--cli", type=Path, help="Reuse an existing verified tree-sitter 0.26.5 binary.")
    for name in ("native", "compare"):
        sub = commands.add_parser(name)
        sub.add_argument("--output", type=Path, required=True)
        if name == "compare":
            sub.add_argument("--candidate-library", type=Path)
            sub.add_argument("--case", action="append", dest="cases")
    for sub in commands.choices.values():
        sub.add_argument("--cache", type=Path, default=build.DEFAULT_CACHE)
    args = cli.parse_args()
    try:
        cache = args.cache.resolve()
        if args.command == "prepare":
            lanes = build.prepare(cache, args.upstream_source, args.patch, args.cli)
            print(json.dumps({name: {"cached": lane["cached"], "library": lane["library"]} for name, lane in lanes.items()}))
            return 0
        if args.command == "native":
            return 0 if build.native(cache, args.output.resolve()) else 1
        report = compare(cache, args.output.resolve(), args.candidate_library, args.cases)
        print(json.dumps(report["summary"]))
        return 0 if report["summary"]["candidate_passed"] == report["summary"]["cases"] else 1
    except (build.BenchError, OSError, ValueError, KeyError, AttributeError) as error:
        print(f"cpp-parser-lab: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
