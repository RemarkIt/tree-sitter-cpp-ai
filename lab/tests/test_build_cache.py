import json
from pathlib import Path

import pytest

import cpp_build as build


def fake_tools(monkeypatch):
    calls = []

    def execute(argv, cwd, log, timeout=300):
        calls.append(argv)
        if argv[:2] == ["cc", "--version"]:
            return "controlled compiler"
        if argv[:2] in (["node", "--version"], ["npm", "--version"]):
            return "controlled version"
        if argv[:2] == ["git", "clone"]:
            checkout = Path(argv[-1])
            (checkout / "src").mkdir(parents=True)
            for name in ("grammar.js", "src/parser.c", "src/scanner.c"):
                (checkout / name).write_text(name)
            (checkout / "node_modules/tree-sitter-cli").mkdir(parents=True)
            (checkout / "node_modules/tree-sitter-cli/tree-sitter").write_bytes(b"controlled CLI")
            (checkout / "package-lock.json").write_text(json.dumps({"packages": {
                "node_modules/tree-sitter-cli": {"version": "0.26.5"},
                "node_modules/tree-sitter-c": {"version": "0.24.1"},
            }}))
        if argv[:2] == ["cc", "-dynamiclib"]:
            Path(argv[-1]).write_bytes(b"controlled library")
        return ""

    monkeypatch.setattr(build.platform, "system", lambda: "Darwin")
    monkeypatch.setattr(build, "run", execute)
    return calls


def test_unchanged_builds_reuse_libraries_without_generation(tmp_path, monkeypatch):
    calls = fake_tools(monkeypatch)
    patch = tmp_path / "candidate.patch"
    patch.write_text("controlled patch")
    first = build.prepare(tmp_path / "cache", "local-upstream", patch)
    assert all(not lane["cached"] for lane in first.values())
    calls.clear()
    second = build.prepare(tmp_path / "cache", "local-upstream", patch)
    assert all(lane["cached"] for lane in second.values())
    assert not any("generate" in call or "-dynamiclib" in call or "ci" in call for call in calls)
    assert {name: lane["library_sha256"] for name, lane in first.items()} == {
        name: lane["library_sha256"] for name, lane in second.items()
    }


def test_modified_cached_sources_fail_without_overwriting(tmp_path, monkeypatch):
    fake_tools(monkeypatch)
    patch = tmp_path / "candidate.patch"
    patch.write_text("controlled patch")
    lanes = build.prepare(tmp_path / "cache", "local-upstream", patch)
    grammar = Path(lanes["baseline"]["checkout"]) / "grammar.js"
    grammar.write_text("unreviewed edit")
    with pytest.raises(build.BenchError, match="Modified cached build"):
        build.prepare(tmp_path / "cache", "local-upstream", patch)
    assert grammar.read_text() == "unreviewed edit"


def test_failed_install_is_retained_and_next_attempt_can_complete(tmp_path, monkeypatch):
    fake_tools(monkeypatch)
    execute = build.run
    failed = False

    def interrupted(argv, cwd, log, timeout=300):
        nonlocal failed
        if argv[:2] == ["npm", "ci"] and not failed:
            failed = True
            raise build.BenchError("controlled download failure")
        return execute(argv, cwd, log, timeout)

    monkeypatch.setattr(build, "run", interrupted)
    patch = tmp_path / "candidate.patch"
    patch.write_text("controlled patch")
    cache = tmp_path / "cache"
    with pytest.raises(build.BenchError, match="download failure"):
        build.prepare(cache, "local-upstream", patch)
    lanes = build.prepare(cache, "local-upstream", patch)
    assert all(Path(lane["library"]).is_file() for lane in lanes.values())
    retained = list((cache / "incomplete").iterdir())
    assert len(retained) == 1
    assert (retained[0] / "checkout/grammar.js").is_file()
