"""Pinned native builds and tool execution for the C/C++ parser laboratory."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
import uuid
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
LAB = Path(__file__).resolve().parent
DEFAULT_CACHE = Path(os.environ.get("CPP_LAB_CACHE") or ROOT / ".cache" / "lab")
UPSTREAM = "https://github.com/tree-sitter/tree-sitter-cpp.git"
REVISION = "c009222808634c1014f82438d4883753516a2c24"


class BenchError(RuntimeError):
    """An invalid experiment or failed tool invocation, not a parser verdict."""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def run(argv: list[str], cwd: Path, log: Path, timeout: int = 300) -> str:
    log.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            argv, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        log.write_text(f"{argv!r}\n{error}\n", encoding="utf-8")
        raise BenchError(f"Cannot execute {argv[0]}: {error}; log: {log}") from error
    log.write_text(
        f"argv: {json.dumps(argv)}\ncwd: {cwd}\nexit: {result.returncode}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}", encoding="utf-8",
    )
    if result.returncode:
        raise BenchError(f"{argv[0]} exited {result.returncode}; log: {log}")
    return result.stdout


def source_hashes(checkout: Path) -> dict[str, str]:
    names = ["grammar.js", "package-lock.json", "src/scanner.c", "src/parser.c"]
    # Scanner modules included by scanner.c; checkouts without them (upstream) keep their identity.
    names += [path.relative_to(checkout).as_posix() for path in sorted((checkout / "src/scanner").glob("*.h"))]
    return {name: digest(checkout / name) for name in names}


def prepare(cache: Path, source: str, patch: Path, cli_binary: Path | None = None) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise BenchError("The native build recipe currently supports macOS only.")
    patch = patch.resolve(strict=True)
    cache.mkdir(parents=True, exist_ok=True)
    compiler = run(["cc", "--version"], ROOT, cache / "logs" / "compiler.log")
    node = run(["node", "--version"], ROOT, cache / "logs" / "node.log").strip()
    npm = run(["npm", "--version"], ROOT, cache / "logs" / "npm.log").strip()
    if cli_binary is not None:
        cli_binary = cli_binary.resolve(strict=True)
        cli_version = run([str(cli_binary), "--version"], ROOT, cache / "logs" / "cli.log").split()
        if cli_version[:2] != ["tree-sitter", "0.26.5"]:
            raise BenchError(f"Expected tree-sitter 0.26.5, got {cli_version}")
        cli_hash = digest(cli_binary)
        cached_cli = cache / "tools" / cli_hash / "tree-sitter"
        cached_cli.parent.mkdir(parents=True, exist_ok=True)
        if not cached_cli.exists():
            shutil.copy2(cli_binary, cached_cli)
        if digest(cached_cli) != cli_hash:
            raise BenchError(f"Modified cached CLI: {cached_cli}")
        cli_binary = cached_cli
    lanes: dict[str, Any] = {}
    for name, selected_patch in (("baseline", None), ("candidate", patch)):
        patch_hash = digest(selected_patch) if selected_patch else None
        key = hashlib.sha256(json.dumps({
            "revision": REVISION, "patch": patch_hash, "compiler": compiler,
            "node": node, "npm": npm, "platform": platform.platform(),
            "recipe": digest(Path(__file__)),
            "cli_sha256": digest(cli_binary) if cli_binary else None,
        }, sort_keys=True).encode()).hexdigest()
        directory = cache / "builds" / key
        checkout = directory / "checkout"
        library = directory / "cpp.dylib"
        receipt = directory / "build.json"
        if receipt.exists():
            saved = json.loads(receipt.read_text(encoding="utf-8"))
            if (
                saved["key"] == key and library.is_file()
                and digest(library) == saved["library_sha256"]
                and source_hashes(checkout) == saved["source_hashes"]
                and Path(saved["cli"]).is_file() and digest(Path(saved["cli"])) == saved["cli_sha256"]
            ):
                lanes[name] = saved | {"cached": True}
                continue
            raise BenchError(f"Modified cached build: {directory}; choose a fresh --cache.")
        if directory.exists():
            attempt = directory / "attempt.json"
            if directory.is_symlink() or not attempt.is_file() or json.loads(attempt.read_text())["key"] != key:
                raise BenchError(f"Unowned incomplete build: {directory}; choose a fresh --cache.")
            retained = cache / "incomplete" / f"{key}-{uuid.uuid4().hex}"
            retained.parent.mkdir(parents=True, exist_ok=True)
            directory.rename(retained)
        directory.mkdir(parents=True, exist_ok=True)
        write_json(directory / "attempt.json", {"key": key})
        run(["git", "clone", "--no-hardlinks", "--no-checkout", source, str(checkout)],
            ROOT, directory / "clone.log")
        run(["git", "checkout", "--detach", REVISION], checkout, directory / "checkout.log")
        if selected_patch:
            run(["git", "apply", "--check", str(selected_patch)], checkout, directory / "patch-check.log")
            run(["git", "apply", str(selected_patch)], checkout, directory / "patch.log")
        lock = json.loads((checkout / "package-lock.json").read_text(encoding="utf-8"))
        versions = {
            package: lock["packages"][f"node_modules/{package}"]["version"]
            for package in ("tree-sitter-cli", "tree-sitter-c")
        }
        if versions != {"tree-sitter-cli": "0.26.5", "tree-sitter-c": "0.24.1"}:
            raise BenchError(f"Unexpected generation dependencies: {versions}")
        run(["npm", "ci", *(["--ignore-scripts"] if cli_binary else [])],
            checkout, directory / "dependencies.log", timeout=900)
        cli = cli_binary or checkout / "node_modules" / "tree-sitter-cli" / "tree-sitter"
        run([str(cli), "generate"], checkout, directory / "generate.log", timeout=900)
        # Optimize like packaged builds (the Python extension builds with -O2), so timing reflects real use.
        run(["cc", "-dynamiclib", "-O2", "-I", "src", "src/parser.c", "src/scanner.c", "-o", str(library)],
            checkout, directory / "compile.log", timeout=300)
        saved = {
            "key": key, "revision": REVISION, "patch_sha256": patch_hash,
            "library": str(library.resolve()), "library_sha256": digest(library),
            "checkout": str(checkout.resolve()), "source_hashes": source_hashes(checkout),
            "generation_dependencies": versions, "compiler": compiler, "node": node, "npm": npm,
            "cli": str(cli.resolve()), "cli_sha256": digest(cli.resolve()),
        }
        write_json(receipt, saved)
        lanes[name] = saved | {"cached": False}
    write_json(cache / "builds.json", lanes)
    return lanes


def builds(cache: Path) -> dict[str, Any]:
    path = cache / "builds.json"
    if not path.is_file():
        raise BenchError(f"No build receipt at {path}; run prepare first.")
    lanes = json.loads(path.read_text(encoding="utf-8"))
    for name in ("baseline", "candidate"):
        lane = lanes[name]
        library = Path(lane["library"])
        if not library.is_file() or digest(library) != lane["library_sha256"]:
            raise BenchError(f"{name} library changed or is missing: {library}")
        if source_hashes(Path(lane["checkout"])) != lane["source_hashes"]:
            raise BenchError(f"{name} generated sources changed; prepare a fresh build.")
        if digest(Path(lane["cli"])) != lane["cli_sha256"]:
            raise BenchError(f"{name} CLI changed; prepare a fresh build.")
    return lanes


def resolve_library(value: str, cache: Path) -> Path:
    """A parser library path, or the alias `upstream` (prepared baseline) or `dev` (latest editable build)."""
    if value == "upstream":
        return Path(builds(cache)["baseline"]["library"])
    if value == "dev":
        receipt = cache / "dev" / "build.json"
        if not receipt.is_file():
            raise BenchError("No editable build; run cpp_dev.py build first.")
        return Path(json.loads(receipt.read_text(encoding="utf-8"))["library"])
    return Path(value).resolve(strict=True)


def native(cache: Path, output: Path) -> bool:
    results = {}
    for name, lane in builds(cache).items():
        checkout = Path(lane["checkout"])
        log = output.parent / f"native-{name}.log"
        try:
            text = run(
                [lane["cli"], "test", "--grammar-path", str(checkout)],
                checkout, log, timeout=900,
            )
        except BenchError as error:
            results[name] = {"passed": False, "error": str(error), "log": str(log)}
        else:
            results[name] = {"passed": True, "log": str(log), "summary": text.splitlines()[-1:]}
    write_json(output, {"lanes": results, "builds": builds(cache)})
    return all(result["passed"] for result in results.values())
