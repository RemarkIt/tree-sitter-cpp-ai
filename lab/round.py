"""One-shot round check: build, native tests, lab tests, gate, corpus census against the previous
round's library and optional timing, ending in a short verdict.

Exit codes: 0 accept (every check passed and a corpus improved), 1 fail (a hard check failed),
2 review (nothing hard failed, but a change needs a decision), 3 the check could not run.
Full evidence stays under <cache>/rounds/<name>/; private corpus paths come from
<cache>/rounds/config.json or --manifest, never from the repository.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path
from typing import Any

import cpp_build as build

# Names that appear when a misreading turns a macro, keyword, calling convention, annotation or member
# initializer into a declaration; their loss usually means a misreading was fixed.
JUNK_NAME = re.compile(
    r"^([A-Z_][A-Z0-9_]*|try|catch|if|else|for|while|switch|return|override|final|__cdecl|__stdcall|"
    r"__fastcall|__thiscall|m_\w+|_\w+_)$"
)
LISTED = 12


def unmatched_declarations(old: list[dict[str, Any]], new: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Old declarations with no same-named counterpart in new: exact span and kind first, then an
    overlapping span of the same kind, then any overlapping span. Moves and reshapes are not losses."""
    remaining: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for symbol in new:
        remaining[symbol["name"]].append(symbol)
    pending = list(old)
    tests = (
        lambda a, b: a["kind"] == b["kind"] and a["start"] == b["start"] and a["end"] == b["end"],
        lambda a, b: a["kind"] == b["kind"] and b["start"] < a["end"] and a["start"] < b["end"],
        lambda a, b: b["start"] < a["end"] and a["start"] < b["end"],
    )
    for test in tests:
        unmatched = []
        for symbol in pending:
            match = next((other for other in remaining[symbol["name"]] if test(symbol, other)), None)
            if match is None:
                unmatched.append(symbol)
            else:
                remaining[symbol["name"]].remove(match)
        pending = unmatched
    return pending


def source_line(root: Path, path: str, offset: int) -> tuple[int, str]:
    try:
        data = (root / path).read_bytes()
    except OSError:
        return 0, ""
    number = data[:offset].count(b"\n") + 1
    start = data.rfind(b"\n", 0, offset) + 1
    end = data.find(b"\n", offset)
    return number, data[start:len(data) if end < 0 else end].decode("utf-8", "replace").strip()[:110]


def corpus_summary(report: dict[str, Any], source_root: Path | None = None) -> dict[str, Any]:
    """Candidate against the previous library: error counts, broken and changed files, declaration losses."""
    def counts(lane: str) -> dict[str, int]:
        syntax = report["syntax"][lane]
        return {"files": syntax["error_files"], "sites": sum(len(entry["diagnostics"][lane]) for entry in report["files"]),
                "bytes": syntax["error_region_bytes"]}

    comparison = report["comparisons"]["previous"]
    clean_changed = [
        entry["path"] for entry in report["files"]
        if not entry["syntax_error"]["candidate"] and not entry["syntax_error"]["previous"]
        and entry["tree_sha256"]["candidate"] != entry["tree_sha256"]["previous"]
    ]
    lost, gained, junk = [], [], 0
    for entry in report["files"]:
        projection = entry["projection"]
        for symbol in unmatched_declarations(projection["previous"], projection["candidate"]):
            if JUNK_NAME.match(symbol["name"]):
                junk += 1
                continue
            row = {"path": entry["path"], "kind": symbol["kind"], "name": symbol["name"], "start": symbol["start"]}
            if source_root is not None:
                row["line"], row["text"] = source_line(source_root, entry["path"], symbol["start"])
            lost.append(row)
        gained.extend(
            {"path": entry["path"], "kind": symbol["kind"], "name": symbol["name"]}
            for symbol in unmatched_declarations(projection["candidate"], projection["previous"])
            if not JUNK_NAME.match(symbol["name"])
        )
    return {
        "previous": counts("previous"), "candidate": counts("candidate"),
        "newly_broken": comparison["newly_broken"], "newly_clean": comparison["newly_clean"],
        "changed_trees": len(comparison["changed_trees"]), "clean_changed": clean_changed,
        "lost": lost, "gained": gained, "junk_lost": junk,
    }


def verdict(results: dict[str, Any]) -> tuple[str, list[str]]:
    """ACCEPT, FAIL or REVIEW with the reasons; hard failures outrank review items."""
    failures, reviews = [], []
    for step in ("native", "labtests", "gate", "timing"):
        if step in results and not results[step]["passed"]:
            failures.append(f"{step} failed")
    improved, worsened = False, False
    for name, corpus in results.get("corpora", {}).items():
        if corpus["newly_broken"]:
            failures.append(f"{name}: {len(corpus['newly_broken'])} newly broken")
        if corpus["clean_changed"]:
            reviews.append(f"{name}: {len(corpus['clean_changed'])} clean files changed")
        if corpus["lost"]:
            reviews.append(f"{name}: {len(corpus['lost'])} declarations lost")
        before, after = corpus["previous"], corpus["candidate"]
        worse = [key for key in before if after[key] > before[key]]
        if worse:
            worsened = True
            reviews.append(f"{name}: {'/'.join(worse)} increased")
        improved = improved or any(after[key] < before[key] for key in before)
    if not improved and not worsened:
        reviews.append("no corpus improvement")
    return ("FAIL" if failures else "REVIEW" if reviews else "ACCEPT"), failures + reviews


def resolve(value: str, cache: Path) -> Path:
    pinned = cache / "rounds" / f"{value}.dylib"
    return pinned if pinned.is_file() else build.resolve_library(value, cache)


def manifests(cache: Path, overrides: list[str]) -> dict[str, Path]:
    config = cache / "rounds" / "config.json"
    named = json.loads(config.read_text())["manifests"] if config.is_file() else {}
    for value in overrides:
        name, separator, path = value.partition("=")
        if not separator:
            raise build.BenchError(f"--manifest takes NAME=PATH, got {value}")
        named[name] = path
    if not named:
        raise build.BenchError(f"No corpus manifests; pass --manifest NAME=PATH or write {config}")
    return {name: Path(path).expanduser().resolve(strict=True) for name, path in named.items()}


def timing_corpus(cache: Path) -> str | None:
    config = cache / "rounds" / "config.json"
    return json.loads(config.read_text()).get("timing") if config.is_file() else None


def start(argv: list[str], cwd: Path, log: Path) -> tuple[subprocess.Popen[str], Path, float]:
    log.parent.mkdir(parents=True, exist_ok=True)
    handle = log.open("w", encoding="utf-8")
    return subprocess.Popen(argv, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT, text=True), log, time.monotonic()


def finish(job: tuple[subprocess.Popen[str], Path, float], timeout: int = 1800) -> tuple[bool, str, float]:
    process, log, began = job
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    text = log.read_text(encoding="utf-8", errors="replace")
    return process.returncode == 0, text, time.monotonic() - began


def native_summary(passed: bool, text: str) -> str:
    failures = [line.strip() for line in re.sub(r"\x1b\[[0-9;]*m", "", text).splitlines() if "✗" in line]
    if passed:
        return f"{text.count('✓')} cases"
    return f"{len(failures)} failing: " + "; ".join(failures[:5])


def check(args: argparse.Namespace) -> int:
    import bench
    import census
    import cpp_dev

    cache, checkout = args.cache.resolve(), build.ROOT
    rounds = cache / "rounds"
    if args.name == args.previous:
        raise build.BenchError("The round name must differ from --previous.")
    previous = resolve(args.previous, cache)
    corpora = manifests(cache, args.manifest)
    directory = rounds / args.name
    directory.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {"name": args.name, "previous": {"name": args.previous, "library": str(previous)}}
    timings: dict[str, float] = {}

    began = started = time.monotonic()
    try:
        lane = cpp_dev.compile_editable(cache, checkout)
    except build.BenchError as error:
        log = next(iter(sorted((cache / "dev" / "builds").glob("*/generate.log"), key=lambda p: p.stat().st_mtime)[-1:]), None)
        tail = log.read_text(errors="replace").splitlines()[-15:] if log else []
        print(f"round {args.name}: FAIL\nbuild    FAIL {error}")
        print("\n".join(f"   {line}" for line in tail))
        return 1
    timings["build"] = time.monotonic() - began
    library = rounds / f"{args.name}.dylib"
    shutil.copy2(lane["library"], library)
    # Lab tests read the committed generated sources, so refresh them before testing (cpp_dev generated --write).
    workspace = Path(lane["workspace"])
    drift = cpp_dev.generated_drift(checkout, workspace)
    for name in drift:
        (checkout / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(workspace / name, checkout / name)
    results["build"] = {"library": str(library), "cached": lane["cached"], "generated_refreshed": drift}

    # Both commands run `tree-sitter test`, which compiles into one shared per-language library cache,
    # so they run one after the other, beside the in-process gate and census.
    outcomes: dict[str, tuple[bool, str, float]] = {}
    commands = {
        "native": ([lane["cli"], "test", "--grammar-path", str(workspace)], workspace),
        "labtests": ([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", "lab/tests"], checkout),
    }

    def background() -> None:
        for step, (argv, cwd) in commands.items():
            outcomes[step] = finish(start(argv, cwd, directory / f"{step}.log"))

    worker = threading.Thread(target=background)
    worker.start()

    began = time.monotonic()
    report = bench.compare(cache, directory / "gate.json", library, None)
    gate = cpp_dev.regression_verdict(report)
    passing = sorted(row["id"] for row in report["cases"] if row["checks"]["candidate"]["passed"])
    earlier = rounds / args.previous / "summary.json"
    before = set(json.loads(earlier.read_text())["gate"]["passing"]) if earlier.is_file() else None
    results["gate"] = {
        "passed": gate["regression_passed"], "regressions": gate["regression_failures"],
        "count": len(passing), "cases": len(report["cases"]), "passing": passing,
        "newly_passing": sorted(set(passing) - before) if before is not None else None,
        "newly_failing": sorted(before - set(passing)) if before is not None else None,
    }
    timings["gate"] = time.monotonic() - began

    results["corpora"] = {}
    for name, manifest in corpora.items():
        began = time.monotonic()
        report = census.measure(cache, manifest, directory / f"census-{name}.json", library, previous)
        results["corpora"][name] = corpus_summary(report, manifest.parent / "source") | {
            "source": str(manifest.parent / "source")}
        timings[f"census-{name}"] = time.monotonic() - began

    worker.join()
    for step, (passed, text, seconds) in outcomes.items():
        timings[step] = seconds
        tail = [line for line in text.strip().splitlines() if line.strip()][-1:] or [""]
        results[step] = {"passed": passed,
                         "summary": native_summary(passed, text) if step == "native" else tail[0].strip()}

    if args.timing is not None:
        began = time.monotonic()
        results["timing"] = measure_timing(cache, corpora, args.timing, library, directory)
        timings["timing"] = time.monotonic() - began

    results["seconds"] = {key: round(value) for key, value in timings.items()}
    results["wall_seconds"] = round(time.monotonic() - started)
    return conclude(results, directory)


def measure_timing(cache: Path, corpora: dict[str, Path], choice: str | None, library: Path,
                   directory: Path) -> dict[str, Any]:
    import census

    name = choice or timing_corpus(cache) or next(iter(corpora))
    if name not in corpora:
        raise build.BenchError(f"Unknown timing corpus {name}; configured: {sorted(corpora)}")
    summary = census.timing(cache, corpora[name], directory / f"timing-{name}.json", library)["summary"]
    return {
        "passed": summary["passed"], "corpus": name,
        "ratios": {key: round(value["ratio"], 3) for key, value in summary["comparisons"]["baseline"].items()},
        "work": {key: round(value["ratio"], 3) for key, value in summary["parse_work"]["cases"].items()},
    }


def timing_only(args: argparse.Namespace) -> int:
    """Time an already checked round's pinned library and fold the result into its verdict."""
    cache = args.cache.resolve()
    directory = cache / "rounds" / args.name
    summary = directory / "summary.json"
    if not summary.is_file():
        raise build.BenchError(f"No checked round {args.name}; run check first.")
    results = json.loads(summary.read_text())
    began = time.monotonic()
    results["timing"] = measure_timing(cache, manifests(cache, args.manifest), args.corpus,
                                       cache / "rounds" / f"{args.name}.dylib", directory)
    results["seconds"]["timing"] = round(time.monotonic() - began)
    results["wall_seconds"] = results["seconds"]["timing"]
    return conclude(results, directory)


def conclude(results: dict[str, Any], directory: Path) -> int:
    status, reasons = verdict(results)
    results.update(status=status, reasons=reasons)
    build.write_json(directory / "summary.json", results)
    print(render(results, directory))
    return {"ACCEPT": 0, "FAIL": 1, "REVIEW": 2}[status]


def render(results: dict[str, Any], directory: Path) -> str:
    mark = lambda ok: "ok  " if ok else "FAIL"  # noqa: E731
    status, reasons = results["status"], results["reasons"]
    lines = [f"round {results['name']} vs {results['previous']['name']}: {status}"
             + (f" ({'; '.join(reasons)})" if reasons else "")]
    built = results["build"]
    lines.append(f"build    ok   {'cached' if built['cached'] else 'built'}; "
                 f"generated sources refreshed: {built['generated_refreshed'] or 'none'}")
    lines.append(f"native   {mark(results['native']['passed'])} {results['native']['summary']}")
    lines.append(f"labtests {mark(results['labtests']['passed'])} {results['labtests']['summary']}")
    gate_row = results["gate"]
    change = ("" if gate_row["newly_passing"] is None
              else f"; newly passing {gate_row['newly_passing'] or 'none'}; newly failing {gate_row['newly_failing'] or 'none'}")
    lines.append(f"gate     {mark(gate_row['passed'])} {gate_row['count']}/{gate_row['cases']}; "
                 f"regressions {gate_row['regressions'] or 'none'}{change}")
    for name, corpus in results["corpora"].items():
        before, after = corpus["previous"], corpus["candidate"]
        delta = " ".join(f"{key} {before[key]}→{after[key]}" for key in before)
        lines.append(f"{name:<8} {mark(not corpus['newly_broken'])} {delta} | newly broken {len(corpus['newly_broken'])}, "
                     f"newly clean {len(corpus['newly_clean'])}, clean changed {len(corpus['clean_changed'])}, "
                     f"changed trees {corpus['changed_trees']}, lost {len(corpus['lost'])}, gained {len(corpus['gained'])}, "
                     f"junk lost {corpus['junk_lost']}")
        for path in corpus["newly_broken"][:LISTED]:
            lines.append(f"   newly broken  {path}")
        for path in corpus["clean_changed"][:LISTED]:
            lines.append(f"   clean changed {path}")
        for row in corpus["lost"][:LISTED]:
            lines.append(f"   lost {row['kind']} {row['name']}  {row['path']}:{row.get('line', '?')}  {row.get('text', '')}")
    if "timing" in results:
        row = results["timing"]
        lines.append(f"timing   {mark(row['passed'])} {row['corpus']}: "
                     + " ".join(f"{key.removesuffix('_ns')} {value}" for key, value in row["ratios"].items())
                     + " | work " + " ".join(str(value) for value in row["work"].values()))
    lines.append(f"evidence {directory}  ({results['wall_seconds']}s)")
    return "\n".join(lines)


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    cli.add_argument("--cache", type=Path, default=build.DEFAULT_CACHE)
    commands = cli.add_subparsers(dest="command", required=True)
    run = commands.add_parser("check", help="Build the working tree and check it against the previous round.")
    run.add_argument("--name", required=True, help="this round's name; its library is pinned as rounds/<name>.dylib")
    run.add_argument("--previous", required=True,
                     help="earlier round name (rounds/<name>.dylib), library path, or alias dev/upstream")
    run.add_argument("--manifest", action="append", default=[], metavar="NAME=PATH",
                     help="census manifest; default: rounds/config.json {\"manifests\": {NAME: PATH}}")
    run.add_argument("--timing", nargs="?", const="", metavar="NAME",
                     help="also time the candidate against upstream on corpus NAME "
                          "(default: config.json \"timing\", else the first corpus)")
    timed = commands.add_parser("timing", help="Time a checked round's pinned library and update its verdict.")
    timed.add_argument("--name", required=True)
    timed.add_argument("--corpus", help="corpus to time (default: config.json \"timing\", else the first corpus)")
    timed.add_argument("--manifest", action="append", default=[], metavar="NAME=PATH")
    args = cli.parse_args()
    try:
        return check(args) if args.command == "check" else timing_only(args)
    except (build.BenchError, OSError, ValueError, KeyError) as error:
        print(f"cpp-parser-round: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
