"""Hash-checked corpus census and timing; function-node counts are not symbol recall."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict
import hashlib
import json
import math
import os
import statistics
import sys
import time
from pathlib import Path
from typing import Any

from tree_sitter import Node

import bench
import cpp_build as build


def manifest_files(manifest_path: Path, expected_files: int | None = None) -> list[tuple[str, bytes]]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    root = (manifest_path.parent / "source").resolve(strict=True)
    files: list[tuple[str, bytes]] = []
    seen = set()
    for entry in manifest["files"]:
        relative = entry["path"]
        path = root / relative
        if (
            relative in seen or Path(relative).is_absolute()
            or path.is_symlink() or not path.resolve(strict=True).is_relative_to(root)
        ):
            raise build.BenchError(f"Unsafe or duplicate manifest path: {relative}")
        seen.add(relative)
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["current_sha256"]:
            raise build.BenchError(f"Source hash mismatch: {relative}")
        files.append((relative, data))
    if not files:
        raise build.BenchError("An empty manifest is not corpus evidence.")
    if expected_files is not None and len(files) != expected_files:
        raise build.BenchError(f"Expected {expected_files} frozen files, found {len(files)}.")
    return files


def hides_missing_token(node: Node) -> bool:
    """Hidden tokens are not exposed as nodes; an erroneous node whose visible children are all
    error-free therefore contains a hidden missing token."""
    return (node.type != "ERROR" and not node.is_missing and node.has_error
            and not any(child.has_error for child in node.children))


def diagnostics(root: Node) -> list[dict[str, Any]]:
    """ERROR and missing nodes in preorder. A hidden missing token is reported at the range of
    the visible node containing it, with `within` naming that node."""
    found, pending = [], [root]
    while pending:
        node = pending.pop()
        if node.type == "ERROR" or node.is_missing:
            found.append({"kind": node.type, "start": node.start_byte, "end": node.end_byte, "missing": node.is_missing})
        elif hides_missing_token(node):
            found.append({"kind": "hidden", "start": node.start_byte, "end": node.end_byte, "missing": True,
                          "within": node.type})
        pending.extend(reversed(node.children))
    return found


def uncertain_regions(root: Node) -> list[dict[str, Any]]:
    return [
        {"kind": node.type, "start": node.start_byte, "end": node.end_byte}
        for node in bench.walk(root)
        if node.type in {"preproc_call", "preproc_function_def"} or bench.is_uncertain(node)
    ]


def declaration_delta(before: list[dict[str, Any]], after: list[dict[str, Any]]) -> dict[str, Any]:
    left = Counter(json.dumps(value, sort_keys=True) for value in before)
    right = Counter(json.dumps(value, sort_keys=True) for value in after)
    return {
        "disappeared": [json.loads(value) for value in sorted((left - right).elements())],
        "appeared": [json.loads(value) for value in sorted((right - left).elements())],
    }


def measure(
    cache: Path, manifest_path: Path, output: Path, candidate_library: Path | None = None,
    previous_library: Path | None = None, expected_files: int | None = None,
) -> dict[str, Any]:
    sources = manifest_files(manifest_path, expected_files)
    lanes = build.builds(cache)
    libraries = {
        "baseline": Path(lanes["baseline"]["library"]),
        "candidate": candidate_library.resolve(strict=True) if candidate_library else Path(lanes["candidate"]["library"]),
    }
    if previous_library:
        libraries["previous"] = previous_library.resolve(strict=True)
    parsers = {name: bench.parser_for(library) for name, library in libraries.items()}
    totals = {"baseline": 0, "candidate": 0, "disappeared": 0, "appeared": 0, "changed_files": 0}
    syntax = {
        name: {"error_files": 0, "error_nodes": 0, "missing_nodes": 0, "error_region_bytes": 0}
        for name in parsers
    }
    syntax_changes: dict[str, list[str]] = {"newly_clean": [], "newly_broken": []}
    files: list[dict[str, Any]] = []
    for relative, data in sources:
        roots = {name: parser.parse(data).root_node for name, parser in parsers.items()}
        if roots["baseline"].has_error and not roots["candidate"].has_error:
            syntax_changes["newly_clean"].append(relative)
        if not roots["baseline"].has_error and roots["candidate"].has_error:
            syntax_changes["newly_broken"].append(relative)
        for name, root in roots.items():
            syntax[name]["error_files"] += int(root.has_error)
            pending = [root]
            while pending:
                node = pending.pop()
                pending.extend(node.children)
                syntax[name]["missing_nodes"] += int(node.is_missing or hides_missing_token(node))
                if node.type == "ERROR":
                    syntax[name]["error_nodes"] += 1
                    parent = node.parent
                    while parent is not None and parent.type != "ERROR":
                        parent = parent.parent
                    if parent is None:
                        syntax[name]["error_region_bytes"] += node.end_byte - node.start_byte
        functions = {
            name: {(node.start_byte, node.end_byte): node for node in bench.walk(root)
                   if node.type == "function_definition"}
            for name, root in roots.items()
        }
        before, after = functions["baseline"], functions["candidate"]
        disappeared, appeared = before.keys() - after.keys(), after.keys() - before.keys()
        for name in ("baseline", "candidate"):
            totals[name] += len(functions[name])
        totals["disappeared"] += len(disappeared)
        totals["appeared"] += len(appeared)
        if disappeared or appeared:
            totals["changed_files"] += 1

        def records(spans: set[tuple[int, int]], lane: str, other: str) -> list[dict[str, Any]]:
            result: list[dict[str, Any]] = []
            for start, end in sorted(spans):
                node = functions[lane][(start, end)]
                declarator = node.child_by_field_name("declarator")
                name_node = bench.named_declarator(declarator) if declarator else None
                counterpart = roots[other].named_descendant_for_byte_range(start, end)
                if counterpart is None:
                    raise build.BenchError(f"{relative}: no {other} node covers function span {start}:{end}")
                ancestors = []
                parent: Node | None = counterpart
                while parent is not None:
                    if parent.type == "ERROR":
                        ancestors.append({"start": parent.start_byte, "end": parent.end_byte})
                    parent = parent.parent
                result.append({
                    "start": start, "end": end, "start_line": node.start_point.row + 1,
                    "end_line": node.end_point.row + 1,
                    "name": bench.text(name_node, data) if name_node else None,
                    "counterpart": {"kind": counterpart.type, "start": counterpart.start_byte, "end": counterpart.end_byte},
                    "error_ancestors": ancestors,
                })
            return result

        files.append({
            "path": relative, "sha256": hashlib.sha256(data).hexdigest(),
            "counts": {name: len(value) for name, value in functions.items()},
            "syntax_error": {name: root.has_error for name, root in roots.items()},
            "tree_sha256": {name: hashlib.sha256(str(root).encode()).hexdigest() for name, root in roots.items()},
            "projection": {
                name: [asdict(symbol) for symbol in bench.tree_symbols(relative, data, root)]
                for name, root in roots.items()
            },
            "uncertainties": {name: uncertain_regions(root) for name, root in roots.items()},
            "diagnostics": {name: diagnostics(root) for name, root in roots.items()},
            "function_nodes": {
                name: [{"start": start, "end": end} for start, end in sorted(value)]
                for name, value in functions.items()
            },
            "disappeared": records(disappeared, "baseline", "candidate"),
            "appeared": records(appeared, "candidate", "baseline"),
        })
    totals["net"] = totals["candidate"] - totals["baseline"]
    for values in syntax.values():
        values["files"] = len(files)
        values["maximum_error_files_for_10_percent"] = (len(files) - 1) // 10
        values["distance_to_target"] = max(0, values["error_files"] - (len(files) - 1) // 10)
        values["syntax_target_met"] = values["distance_to_target"] == 0
    comparisons = {}
    for reference in ("baseline", "previous"):
        if reference not in libraries:
            continue
        comparisons[reference] = {
            "newly_clean": [
                entry["path"] for entry in files
                if entry["syntax_error"][reference] and not entry["syntax_error"]["candidate"]
            ],
            "newly_broken": [
                entry["path"] for entry in files
                if not entry["syntax_error"][reference] and entry["syntax_error"]["candidate"]
            ],
            "declarations": [
                {"path": entry["path"], **delta}
                for entry in files
                if any((delta := declaration_delta(entry["projection"][reference], entry["projection"]["candidate"])).values())
            ],
            "changed_trees": [
                entry["path"] for entry in files
                if entry["tree_sha256"][reference] != entry["tree_sha256"]["candidate"]
            ],
            "changed_uncertainties": [
                entry["path"] for entry in files
                if entry["uncertainties"][reference] != entry["uncertainties"]["candidate"]
            ],
            "changed_diagnostics": [
                entry["path"] for entry in files
                if entry["diagnostics"][reference] != entry["diagnostics"]["candidate"]
            ],
            "function_nodes": [
                {"path": entry["path"], **delta}
                for entry in files
                if any((delta := declaration_delta(entry["function_nodes"][reference], entry["function_nodes"]["candidate"])).values())
            ],
        }
    report = {
        "schema": 2, "manifest_sha256": build.digest(manifest_path),
        "builds": {
            "baseline": lanes["baseline"],
            "candidate": lanes["candidate"] if candidate_library is None else {"origin": "external"},
        },
        "candidate_origin": "external" if candidate_library else "prepared",
        "libraries": {name: {"path": str(path), "sha256": build.digest(path)} for name, path in libraries.items()},
        "syntax": syntax,
        "syntax_changes": syntax_changes,
        "harness_sha256": {name: build.digest(build.LAB / name) for name in ("bench.py", "census.py", "cpp_build.py")},
        "files": files, "totals": totals, "comparisons": comparisons,
        "limitations": [
            "Span replacement is not proof of a lost or newly correct symbol.",
            "These are syntactic function_definition nodes, not compiler-verified corpus recall.",
            "Projection covers functions and variables only; types, aliases, enumerators and expansion-only symbols are unsupported.",
            "Macro/preprocessor regions report syntax, not expansion semantics or active branch selection.",
            "Private source names/locations remain in this local report; do not publish it.",
        ],
    }
    build.write_json(output, report)
    return report


def tree_evidence(path: str, data: bytes, tree: Any) -> dict[str, Any]:
    root = tree.root_node
    return {
        "tree": str(root), "has_error": root.has_error,
        "diagnostics": [
            (entry["kind"], entry["start"], entry["end"], entry["missing"]) for entry in diagnostics(root)
        ],
        "projection": [asdict(symbol) for symbol in bench.tree_symbols(path, data, root)],
        "uncertainties": uncertain_regions(root),
    }


def incremental_edits() -> list[dict[str, Any]]:
    """Disposable independent edits spanning scanner, preprocessor and declaration boundaries."""
    return [
        {"path": "raw-string.cpp", "before": b'int f() { return R"tag(x)tag"[0]; }\n',
         "old": b"x", "new": b")other\\n"},
        {"path": "conditional.c", "before": b"#if FLAG\nint a;\n#else\nint b;\n#endif\n",
         "old": b"int a;", "new": b"int a; /* edited */\nint c;"},
        {"path": "declaration.cpp", "before": b"int first() { return 1; }\nint next;\n",
         "old": b"1", "new": b"2 + 3"},
        {"path": "scanner-stress.cpp",
         "before": b'const char *s = R"abcdefghijklmnop(' + b")abcdefghijklmnoX" * 128 + b'UNIQUE)abcdefghijklmnop";\n',
         "old": b"UNIQUE", "new": b")abcdefghijklmnop"},
        {"path": "macro-block.cpp", "before": b"region(1) { int local = 2; }\nint after;\n",
         "old": b"region(1)", "new": b"region((1 + 2)) /* edited */"},
        {"path": "macro-block.c", "before": b"int f() { region(1)\r\n{ int local = 2; } return 0; }\n",
         "old": b"region(1)", "new": b"region(1);"},
        {"path": "macro-literal.cpp", "before": b'region(R"tag(x)tag") { int local; }\n',
         "old": b"x", "new": b")other\\n"},
        {"path": "initializer-control.cpp", "before": b"int f() { int local{call()}; return local; }\n",
         "old": b"call()", "new": b"call((1 + 2))"},
        {"path": "statement-boundary.cpp", "before": b"int f() { emit(1)\nreturn 0; }\n",
         "old": b"emit(1)", "new": b"emit(1);"},
        {"path": "statement-brace.c", "before": b"void f(void) { emit(1)}\r\n",
         "old": b"emit(1)", "new": b"emit((1 + 2));"},
        {"path": "initializer-boundary.cpp", "before": b"int local{call()};\n",
         "old": b"call()", "new": b"call()\n"},
        {"path": "bare-marker.cpp", "before": b"int f() { trace()\nmarker { int saved = 1; } return 0; }\n",
         "old": b"marker", "new": b"if (flag)"},
        {"path": "call-continuation.cpp", "before": b"int f() { call()\n+ 1; return 0; }\n",
         "old": b"\n+", "new": b"\nother() +"},
    ]


def parse_work_cases() -> list[dict[str, Any]]:
    """Ordinary inputs whose GLR work must stay near baseline after an early local ambiguity."""
    tail = b"".join(b"int f%d(int a)\n{\n    return a + %d;\n}\n" % (index, index) for index in range(40))
    return [
        {"path": "ambiguous-head-tail.cpp",
         "source": b"Widget::Widget(Widget& other)\n{\n    reset();\n}\n" + tail},
        {"path": "plain-tail.cpp", "source": tail},
    ]


def parse_steps(parser: Any, data: bytes) -> int:
    """Count per-version token steps; alternatives that never merge multiply this deterministically."""
    steps = 0

    def log(_kind: Any, message: str) -> None:
        nonlocal steps
        if message.startswith("process version:"):
            steps += 1

    parser.logger = log
    try:
        parser.parse(data)
    finally:
        parser.logger = None
    return steps


def parse_work_summary(work: dict[str, dict[str, int]], budget: float = 1.25) -> dict[str, Any]:
    if "baseline" not in work or "candidate" not in work or set(work["baseline"]) != set(work["candidate"]):
        raise build.BenchError("Parse work requires matching baseline and candidate cases.")
    cases = {}
    for path, steps in work["baseline"].items():
        if steps <= 0:
            raise build.BenchError("Parse work needs positive baseline step evidence.")
        ratio = work["candidate"][path] / steps
        cases[path] = {"ratio": ratio, "budget": budget, "passed": ratio <= budget}
    return {"steps": work, "cases": cases, "passed": all(case["passed"] for case in cases.values())}


def edited_tree(parser: Any, case: dict[str, Any]) -> tuple[bytes, Any]:
    before, old, new = case["before"], case["old"], case["new"]
    if before.count(old) != 1:
        raise build.BenchError("Incremental fixture needs exactly one edit site.")
    start = before.index(old)
    end = start + len(old)
    data = before[:start] + new + before[end:]
    def point(content: bytes, offset: int) -> tuple[int, int]:
        prefix = content[:offset]
        return prefix.count(b"\n"), len(prefix.rsplit(b"\n", 1)[-1])
    tree = parser.parse(before)
    tree.edit(start_byte=start, old_end_byte=end, new_end_byte=start + len(new),
              start_point=point(before, start), old_end_point=point(before, end),
              new_end_point=point(data, start + len(new)))
    return data, tree


def timing_summary(samples: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    metrics = {}
    paths: set[str] | None = None
    for name, runs in samples.items():
        if len(runs) != 5 or any(not run["parse_ns"] for run in runs):
            raise build.BenchError("Timing requires five nonempty samples per lane.")
        for run in runs:
            if paths is None:
                paths = set(run["parse_ns"])
            if (
                set(run["parse_ns"]) != paths or any(value <= 0 for value in run["parse_ns"].values())
                or run["parse_projection_ns"] <= 0 or run["incremental_ns"] <= 0
            ):
                raise build.BenchError("Timing needs matching files and positive clock evidence.")
        metrics[name] = {
            "parse_ns": statistics.median(sum(run["parse_ns"].values()) for run in runs),
            "parse_projection_ns": statistics.median(run["parse_projection_ns"] for run in runs),
            "per_file_p95_ns": sorted(
                value for run in runs for value in run["parse_ns"].values()
            )[math.ceil(sum(len(run["parse_ns"]) for run in runs) * .95) - 1],
            "incremental_ns": statistics.median(run["incremental_ns"] for run in runs),
        }
    if "baseline" not in metrics or "candidate" not in metrics:
        raise build.BenchError("Timing requires baseline and candidate evidence.")
    budgets = {"parse_ns": 1.20, "parse_projection_ns": 1.20, "per_file_p95_ns": 1.25, "incremental_ns": 1.25}
    comparisons = {}
    for reference in metrics.keys() - {"candidate"}:
        ratios = {}
        for metric, budget in budgets.items():
            denominator = metrics[reference][metric]
            if denominator <= 0:
                raise build.BenchError("Timing clock produced nonpositive baseline evidence.")
            ratio = metrics["candidate"][metric] / denominator
            ratios[metric] = {"ratio": ratio, "budget": budget, "passed": ratio <= budget}
        comparisons[reference] = ratios
    return {"metrics": metrics, "comparisons": comparisons,
            "passed": all(value["passed"] for value in comparisons["baseline"].values())}


def timing(
    cache: Path, manifest_path: Path, output: Path, candidate_library: Path | None = None,
    previous_library: Path | None = None, expected_files: int | None = None,
    clock: Any = time.perf_counter_ns,
) -> dict[str, Any]:
    sources = manifest_files(manifest_path, expected_files)
    lanes = build.builds(cache)
    libraries = {"baseline": Path(lanes["baseline"]["library"]),
                 "candidate": candidate_library or Path(lanes["candidate"]["library"])}
    if previous_library:
        libraries["previous"] = previous_library
    parsers = {}
    cold = {}
    for name, library in libraries.items():
        start = clock()
        parser = bench.parser_for(library)
        loaded = clock()
        parser.parse(sources[0][1])
        cold[name] = {"library_load_ns": loaded - start, "first_parse_ns": clock() - loaded}
        parsers[name] = parser
    edits = incremental_edits()
    incremental: dict[str, list[dict[str, Any]]] = {}
    for name, parser in parsers.items():
        incremental[name] = []
        for case in edits:
            data, tree = edited_tree(parser, case)
            observed = tree_evidence(case["path"], data, parser.parse(data, tree))
            fresh = tree_evidence(case["path"], data, parser.parse(data))
            if observed != fresh:
                raise build.BenchError(f"Incremental/fresh mismatch: {name}/{case['path']}")
            incremental[name].append({"path": case["path"], "matched": True,
                                      "evidence": observed})
    work = {name: {case["path"]: parse_steps(parser, case["source"]) for case in parse_work_cases()}
            for name, parser in parsers.items()}
    samples: dict[str, list[dict[str, Any]]] = {name: [] for name in parsers}
    orders = []
    for iteration in range(7):
        order = list(parsers) if iteration % 2 == 0 else list(reversed(parsers))
        orders.append(order)
        for name in order:
            parser = parsers[name]
            parsed = {}
            projection_ns = 0
            for path, data in sources:
                start = clock()
                parser.parse(data)
                parsed[path] = clock() - start
                start = clock()
                tree = parser.parse(data)
                bench.tree_symbols(path, data, tree.root_node)
                projection_ns += clock() - start
            incremental_ns = 0
            for _ in range(20):
                for case in edits:
                    data, tree = edited_tree(parser, case)
                    start = clock()
                    parser.parse(data, tree)
                    incremental_ns += clock() - start
            if iteration >= 2:
                samples[name].append({"parse_ns": parsed, "parse_projection_ns": projection_ns,
                                      "incremental_ns": incremental_ns})
    summary = timing_summary(samples)
    summary["parse_work"] = parse_work_summary({name: work[name] for name in ("baseline", "candidate")})
    summary["passed"] = summary["passed"] and summary["parse_work"]["passed"]
    medians = {
        name: {path: statistics.median(run["parse_ns"][path] for run in runs) for path, _ in sources}
        for name, runs in samples.items()
    }
    largest_path, largest_data = max(sources, key=lambda item: len(item[1]))
    worst_path = max(medians["baseline"], key=lambda path: medians["candidate"][path] / medians["baseline"][path])
    report = {
        "schema": 1, "manifest_sha256": build.digest(manifest_path),
        "host": dict(zip(("system", "node", "release", "version", "machine"), os.uname())),
        "binding": "tree-sitter==0.25.2",
        "baseline_toolchain": {key: lanes["baseline"][key] for key in ("compiler", "node", "cli_sha256")},
        "harness_sha256": {name: build.digest(build.LAB / name) for name in ("census.py", "bench.py", "cpp_build.py")},
        "libraries": {name: {"path": str(path), "sha256": build.digest(path), "bytes": path.stat().st_size}
                      for name, path in libraries.items()},
        "inputs": [{"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} for path, data in sources],
        "protocol": {"warmups": 2, "samples": 5, "orders": orders,
                     "incremental_repetitions": 20,
                     "parse_work": "tree-sitter 'process version' log events per independent case",
                     "excludes": ["source I/O", "Clang", "generation/compilation", "report serialization", "Tree.edit setup"]},
        "cold": cold, "samples": samples, "summary": summary, "incremental": incremental,
        "largest_file": {"path": largest_path, "bytes": len(largest_data)},
        "worst_slowdown": {"path": worst_path, "ratio": medians["candidate"][worst_path] / medians["baseline"][worst_path],
                           "baseline_ns": medians["baseline"][worst_path], "candidate_ns": medians["candidate"][worst_path]},
        "decision": "within provisional budgets" if summary["passed"] else "suspected breach; repeat protocol once and retain both reports",
        "limitations": ["Cold fields are process-first load/parse observations; aliases may reuse an already loaded library.",
                        "Macro/uncertain-region counts and bounded declarations are not complete C++ semantic recall."],
    }
    build.write_json(output, report)
    return report


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--manifest", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    cli.add_argument("--cache", type=Path, default=build.DEFAULT_CACHE)
    cli.add_argument("--candidate-library", help="library path or alias (dev, upstream); default: prepared candidate")
    cli.add_argument("--previous-library", help="library path or alias (dev, upstream)")
    cli.add_argument("--expected-files", type=int)
    cli.add_argument("--timing", action="store_true")
    args = cli.parse_args()
    try:
        operation = timing if args.timing else measure
        cache = args.cache.resolve()
        candidate, previous = (build.resolve_library(value, cache) if value else None
                               for value in (args.candidate_library, args.previous_library))
        report = operation(cache, args.manifest.resolve(strict=True), args.output.resolve(),
                           candidate, previous, args.expected_files)
        print(json.dumps({"output": str(args.output), **{
            key: report[key] for key in ("totals", "syntax", "summary") if key in report
        }}))
        return 0
    except (build.BenchError, OSError, ValueError, KeyError) as error:
        print(f"cpp-parser-lab census: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
