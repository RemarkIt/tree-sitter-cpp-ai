"""Materialize a public corpus specification as a hash-checked census manifest.

A specification pins each repository to a commit and lists files by path and SHA-256, so
third-party sources are fetched on demand instead of being redistributed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import cpp_build as build


def fetch_repository(checkout: Path, url: str, commit: str, paths: list[str], logs: Path) -> None:
    if not (checkout / ".git").is_dir():
        checkout.mkdir(parents=True, exist_ok=True)
        build.run(["git", "init", "-q"], checkout, logs / "init.log")
        build.run(["git", "remote", "add", "origin", url], checkout, logs / "remote.log")
    try:
        head = build.run(["git", "rev-parse", "--verify", "-q", "HEAD"], checkout, logs / "head.log").strip()
    except build.BenchError:
        head = ""
    if head == commit and all((checkout / path).is_file() for path in paths):
        return
    # Partial clone plus sparse checkout downloads only the selected blobs at the pinned commit.
    build.run(["git", "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit],
              checkout, logs / "fetch.log", timeout=900)
    build.run(["git", "sparse-checkout", "set", "--no-cone", *(f"/{path}" for path in paths)],
              checkout, logs / "sparse.log")
    build.run(["git", "checkout", "-q", "--detach", commit], checkout, logs / "checkout.log", timeout=900)


def materialize(specification: Path, destination: Path) -> dict[str, Any]:
    spec = json.loads(specification.read_text(encoding="utf-8"))
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for entry in spec["files"]:
        grouped[entry["repo"]].append(entry)
    source = destination / "source"
    files = []
    for name, entries in sorted(grouped.items()):
        repository = spec["repos"][name]
        checkout = destination / "repos" / name
        inner = [entry["path"].removeprefix(f"{name}/") for entry in entries]
        fetch_repository(checkout, repository["url"], repository["commit"], inner, destination / "logs" / name)
        for entry, path in zip(entries, inner):
            data = (checkout / path).read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            if digest != entry["sha256"]:
                raise build.BenchError(f"Source hash mismatch: {entry['path']}")
            target = source / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            files.append({"path": entry["path"], "current_sha256": digest, "bytes": len(data)})
    manifest = {
        "schema": 1, "corpus": spec["name"], "specification_sha256": build.digest(specification),
        "file_count": len(files), "total_bytes": sum(entry["bytes"] for entry in files), "files": files,
    }
    build.write_json(destination / "manifest.json", manifest)
    return manifest


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("specification", type=Path, help="corpus specification, e.g. lab/corpora/heldout.json")
    cli.add_argument("--into", type=Path, help="destination (default: <cache>/corpora/<name>)")
    cli.add_argument("--cache", type=Path, default=build.DEFAULT_CACHE)
    args = cli.parse_args()
    try:
        name = json.loads(args.specification.read_text(encoding="utf-8"))["name"]
        destination = (args.into or args.cache / "corpora" / name).resolve()
        manifest = materialize(args.specification.resolve(strict=True), destination)
        print(json.dumps({"manifest": str(destination / "manifest.json"),
                          "files": manifest["file_count"], "bytes": manifest["total_bytes"]}))
        return 0
    except (build.BenchError, OSError, ValueError, KeyError) as error:
        print(f"cpp-parser-lab corpus: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
