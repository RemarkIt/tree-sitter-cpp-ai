"""Editable grammar development; known gaps are measurements, not accepted behavior."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import sys
import tarfile
from pathlib import Path
from typing import Any

import bench
import cpp_build as build

CHECKOUT = build.ROOT
REQUIRED_INPUTS = {
    "grammar.js", "src/scanner.c", "package.json", "package-lock.json", "tree-sitter.json", "LICENSE",
}
GENERATED = ("src/parser.c", "src/grammar.json", "src/node-types.json",
             "src/tree_sitter/alloc.h", "src/tree_sitter/array.h", "src/tree_sitter/parser.h")


def source_inputs(checkout: Path) -> dict[str, str]:
    """Fingerprint every handwritten generation input; unknown grammar sources fail closed."""
    inputs = {}
    candidates = [checkout / name for name in REQUIRED_INPUTS]
    for directory in ("queries", "test/corpus", "test/highlight", "test/tags"):
        if (checkout / directory).is_dir():
            candidates.extend(sorted((checkout / directory).rglob("*")))
    # The scanner's modules, which scanner.c includes.
    candidates.extend(sorted((checkout / "src/scanner").glob("*.h")))
    # A new grammar module or C source must join the recipe explicitly, never silently.
    unknown = [
        path.relative_to(checkout).as_posix() for path in [*checkout.glob("*.js"), *(checkout / "src").glob("*.[ch]")]
        if path.relative_to(checkout).as_posix() not in REQUIRED_INPUTS | {"src/parser.c"}
    ]
    if unknown:
        raise build.BenchError(f"File outside the supported source recipe: {sorted(unknown)}")
    for path in candidates:
        name = path.relative_to(checkout).as_posix()
        if path.is_symlink():
            raise build.BenchError(f"Symlink grammar input: {name}")
        if not path.is_file():
            continue
        if name.startswith("queries/") and path.suffix != ".scm":
            continue
        inputs[name] = build.digest(path)
    missing = REQUIRED_INPUTS - inputs.keys()
    if missing or not any(name.startswith("test/corpus/") for name in inputs):
        raise build.BenchError(f"Incomplete tracked grammar sources: {sorted(missing)}")
    return dict(sorted(inputs.items()))


def dependency_lock_digest(checkout: Path) -> str:
    """Fingerprint the npm lock's dependencies, ignoring this package's own release version."""
    raw = (checkout / "package-lock.json").read_bytes()
    try:
        lock = json.loads(raw)
    except ValueError:
        return hashlib.sha256(raw).hexdigest()
    if isinstance(lock, dict):
        lock.pop("version", None)
        root = lock.get("packages", {}).get("")
        if isinstance(root, dict):
            root.pop("version", None)
    return hashlib.sha256(json.dumps(lock, sort_keys=True).encode()).hexdigest()


def synchronize(checkout: Path, workspace: Path, inputs: dict[str, str]) -> None:
    if workspace.is_symlink():
        raise build.BenchError(f"Symlink generation workspace: {workspace}")
    manifest = workspace / "inputs.json"
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    for name in old.keys() | inputs.keys():
        path = workspace / name
        if Path(name).is_absolute() or ".." in Path(name).parts or not path.resolve().is_relative_to(workspace.resolve()):
            raise build.BenchError(f"Unsafe owned workspace input: {name}")
        if any(parent.is_symlink() for parent in [path, *path.parents] if parent.is_relative_to(workspace)):
            raise build.BenchError(f"Symlink workspace input: {name}")
    for name in old.keys() - inputs.keys():
        (workspace / name).unlink(missing_ok=True)
    for name in inputs:
        destination = workspace / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or build.digest(destination) != inputs[name]:
            shutil.copy2(checkout / name, destination)
    build.write_json(manifest, inputs)


def initialize(cache: Path, checkout: Path) -> dict[str, Any]:
    inputs = source_inputs(checkout)
    baseline = build.builds(cache)["baseline"]
    directory = cache / "dev"
    workspace = directory / "workspace"
    marker = directory / "setup.json"
    if marker.exists():
        saved = json.loads(marker.read_text())
        if (
            saved.get("schema") != 2 or Path(saved["checkout"]) != checkout
            or Path(saved["workspace"]) != workspace
        ):
            raise build.BenchError(f"Preserve legacy/unmatched development setup before initializing: {marker}")
        if dependency_lock_digest(checkout) != saved["lock_sha256"]:
            raise build.BenchError("Dependency lock changed; use a fresh --cache.")
        if build.digest(Path(saved["cli"])) != saved["cli_sha256"]:
            raise build.BenchError("Development CLI changed; use a fresh --cache.")
    else:
        if workspace.exists():
            raise build.BenchError(f"Refusing to replace an unowned generation workspace: {workspace}")
        saved = {
            "schema": 2, "checkout": str(checkout), "workspace": str(workspace),
            "revision": build.REVISION, "dependencies_ready": False,
            "lock_sha256": dependency_lock_digest(checkout),
            "cli": baseline["cli"], "cli_sha256": baseline["cli_sha256"],
        }
        build.write_json(marker, saved)
    synchronize(checkout, workspace, inputs)
    if not saved["dependencies_ready"] or not (workspace / "node_modules").is_dir():
        build.run(["npm", "ci", "--ignore-scripts"], workspace, directory / "dependencies.log", timeout=900)
    saved["dependencies_ready"] = True
    build.write_json(marker, saved)
    return saved


def development(cache: Path, checkout: Path) -> dict[str, Any]:
    marker = cache / "dev" / "setup.json"
    if not marker.is_file():
        raise build.BenchError("No editable checkout receipt; run cpp_dev.py init first.")
    saved = json.loads(marker.read_text())
    if (
        saved.get("schema") != 2 or Path(saved["checkout"]) != checkout
        or Path(saved["workspace"]) != cache / "dev" / "workspace"
    ):
        raise build.BenchError(f"Tracked sources do not match {marker}; preserve legacy setup and run init.")
    inputs = source_inputs(checkout)
    if dependency_lock_digest(checkout) != saved["lock_sha256"]:
        raise build.BenchError("Editable dependency lock changed; dependency updates require a fresh setup.")
    if build.digest(Path(saved["cli"])) != saved["cli_sha256"]:
        raise build.BenchError("Editable CLI changed; initialize a fresh development environment.")
    workspace = Path(saved["workspace"])
    if not saved["dependencies_ready"] or not (workspace / "node_modules").is_dir():
        raise build.BenchError("Editable grammar dependencies are missing; finish init before building.")
    synchronize(checkout, workspace, inputs)
    return saved


def compile_editable(cache: Path, checkout: Path) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise build.BenchError("The native development build currently supports macOS only.")
    setup = development(cache, checkout)
    workspace = Path(setup["workspace"])
    inputs = source_inputs(checkout)
    compiler = build.run(["cc", "--version"], checkout, cache / "dev" / "compiler.log")
    node = build.run(["node", "--version"], checkout, cache / "dev" / "node.log").strip()
    key = hashlib.sha256(json.dumps({
        "inputs": inputs, "compiler": compiler, "node": node, "platform": platform.platform(),
        "recipe": build.digest(Path(__file__)),
        "cli": setup["cli_sha256"],
    }, sort_keys=True).encode()).hexdigest()
    directory = cache / "dev" / "builds" / key
    directory.mkdir(parents=True, exist_ok=True)
    receipt = directory / "build.json"
    library = directory / "cpp.dylib"
    if receipt.exists():
        saved = json.loads(receipt.read_text())
        if not library.is_file() or build.digest(library) != saved["library_sha256"]:
            raise build.BenchError(f"Modified editable build library: {library}")
        if not (workspace / "src/parser.c").exists() or build.source_hashes(workspace) != saved["source_hashes"]:
            build.run([setup["cli"], "generate"], workspace, directory / "regenerate.log", timeout=900)
            if build.source_hashes(workspace) != saved["source_hashes"]:
                raise build.BenchError(f"Generated source identity changed for cached build: {directory}")
        build.write_json(cache / "dev" / "build.json", saved)
        return saved | {"cached": True}
    build.run([setup["cli"], "generate"], workspace, directory / "generate.log", timeout=900)
    # Optimize like packaged builds (the Python extension builds with -O2), so timing reflects real use.
    build.run(["cc", "-dynamiclib", "-O2", "-I", "src", "src/parser.c", "src/scanner.c", "-o", str(library)],
              workspace, directory / "compile.log", timeout=300)
    saved = {
        "origin": "editable", "key": key, "checkout": str(checkout), "revision": build.REVISION,
        "inputs": inputs, "workspace": str(workspace), "compiler": compiler, "node": node, "cli": setup["cli"],
        "library": str(library), "library_sha256": build.digest(library),
        "source_hashes": build.source_hashes(workspace),
    }
    build.write_json(receipt, saved)
    build.write_json(cache / "dev" / "build.json", saved)
    return saved | {"cached": False}


def regression_verdict(report: dict[str, Any]) -> dict[str, Any]:
    required = {case["id"] for case in bench.fixtures() if case.get("required", False)}
    failures = [
        row["id"] for row in report["cases"]
        if (row["checks"]["baseline"]["passed"] or row["id"] in required)
        and not row["checks"]["candidate"]["passed"]
    ]
    unresolved = [row["id"] for row in report["cases"] if not row["checks"]["candidate"]["passed"]]
    return {
        "regression_passed": not failures, "regression_failures": failures,
        "unresolved_cases": unresolved, "selected_cases_passed": not unresolved,
    }


def generated_drift(checkout: Path, workspace: Path) -> list[str]:
    """Committed generated sources that differ from the pinned CLI's output for the current inputs."""
    return [
        name for name in GENERATED
        if not (checkout / name).is_file() or build.digest(checkout / name) != build.digest(workspace / name)
    ]


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("command", choices=("init", "build", "measure", "gate", "native", "export", "generated"))
    cli.add_argument("--cache", type=Path, default=build.DEFAULT_CACHE)
    cli.add_argument("--checkout", type=Path, default=CHECKOUT)
    cli.add_argument("--output", type=Path)
    cli.add_argument("--case", action="append", dest="cases")
    cli.add_argument("--write", action="store_true", help="generated: refresh the committed generated sources")
    args = cli.parse_args()
    try:
        cache, checkout = args.cache.resolve(), args.checkout.resolve()
        if args.cases and args.command != "measure":
            raise build.BenchError("Only measure accepts focused cases; the regression gate requires all cases.")
        if args.write and args.command != "generated":
            raise build.BenchError("Only generated accepts --write.")
        if args.command == "init":
            print(json.dumps(initialize(cache, checkout)))
            return 0
        if args.command == "export":
            if args.output is None:
                raise build.BenchError("export requires --output")
            development(cache, checkout)
            inputs = source_inputs(checkout)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with tarfile.open(args.output, "w:gz") as archive:
                for name in inputs:
                    archive.add(checkout / name, arcname=name, recursive=False)
            build.write_json(args.output.with_suffix(".inputs.json"), inputs)
            print(json.dumps({"sources": str(args.output), "sha256": build.digest(args.output)}))
            return 0
        lane = compile_editable(cache, checkout)
        if args.command == "build":
            print(json.dumps(lane))
            return 0
        if args.command == "generated":
            workspace = Path(lane["workspace"])
            drift = generated_drift(checkout, workspace)
            if args.write:
                for name in drift:
                    (checkout / name).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(workspace / name, checkout / name)
            print(json.dumps({"drift": drift, "written": args.write}))
            return 0 if args.write or not drift else 1
        output = args.output or cache / "dev" / f"{args.command}.json"
        if args.command == "native":
            workspace = Path(lane["workspace"])
            text = build.run([lane["cli"], "test", "--grammar-path", str(workspace)],
                             workspace, output.with_suffix(".log"), timeout=900)
            build.write_json(output, {"passed": True, "build": lane, "summary": text.splitlines()[-1:]})
            print(json.dumps({"passed": True, "output": str(output)}))
            return 0
        report = bench.compare(cache, output, Path(lane["library"]), args.cases)
        report["builds"]["candidate"] = lane
        report["mode"] = args.command
        report["verdict"] = regression_verdict(report)
        build.write_json(output, report)
        print(json.dumps({"summary": report["summary"], "verdict": report["verdict"], "mode": args.command}))
        if args.command == "gate":
            return 0 if report["verdict"]["regression_passed"] else 1
        return 0
    except (build.BenchError, OSError, ValueError, KeyError) as error:
        print(f"cpp-parser-dev: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
