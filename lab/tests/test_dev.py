import json
from pathlib import Path
import shutil
import subprocess
import tarfile

import pytest

import bench
import cpp_build as build
import cpp_dev as dev


def editable(tmp_path, monkeypatch):
    checkout = tmp_path / "grammar"
    for name in dev.source_inputs(dev.CHECKOUT):
        (checkout / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dev.CHECKOUT / name, checkout / name)
    cli = tmp_path / "cli"
    cli.write_bytes(b"controlled CLI")
    cache = tmp_path / "cache"
    baseline = {"checkout": str(checkout), "cli": str(cli), "cli_sha256": build.digest(cli)}
    monkeypatch.setattr(build, "builds", lambda _cache: {"baseline": baseline})
    calls = []

    def execute(argv, cwd, log, timeout=300):
        calls.append(argv)
        if argv[:2] == ["cc", "--version"]:
            return "controlled compiler"
        if "ci" in argv:
            (cwd / "node_modules").mkdir(exist_ok=True)
        if "generate" in argv:
            for name in dev.GENERATED:
                (cwd / name).parent.mkdir(parents=True, exist_ok=True)
                (cwd / name).write_text(name + (cwd / "grammar.js").read_text())
        if "-dynamiclib" in argv:
            Path(argv[-1]).write_bytes((cwd / "src/parser.c").read_bytes())
        return ""

    monkeypatch.setattr(build, "run", execute)
    monkeypatch.setattr(dev.platform, "system", lambda: "Darwin")
    dev.initialize(cache, checkout)
    calls.clear()
    return cache, checkout, calls


def test_editable_build_reuses_dependencies_and_unchanged_native_library(tmp_path, monkeypatch):
    cache, checkout, calls = editable(tmp_path, monkeypatch)
    first = dev.compile_editable(cache, checkout)
    calls.clear()
    second = dev.compile_editable(cache, checkout)
    assert first["cached"] is False
    assert second["cached"] is True
    assert first["library_sha256"] == second["library_sha256"]
    assert not any("generate" in call or "-dynamiclib" in call or "ci" in call or "clone" in call for call in calls)


def test_edit_rebuilds_into_a_distinct_library_without_cloning_or_installing(tmp_path, monkeypatch):
    cache, checkout, calls = editable(tmp_path, monkeypatch)
    first = dev.compile_editable(cache, checkout)
    (checkout / "grammar.js").write_text("changed grammar")
    calls.clear()
    second = dev.compile_editable(cache, checkout)
    assert first["library"] != second["library"]
    assert Path(first["library"]).is_file()
    assert second["cached"] is False
    assert any("generate" in call for call in calls)
    assert not any("clone" in call or "ci" in call for call in calls)


def test_modified_native_library_is_rejected_without_overwriting(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    lane = dev.compile_editable(cache, checkout)
    library = Path(lane["library"])
    library.write_bytes(b"unreviewed replacement")
    with pytest.raises(build.BenchError, match="Modified editable build"):
        dev.compile_editable(cache, checkout)
    assert library.read_bytes() == b"unreviewed replacement"


def test_repository_metadata_is_neither_an_input_nor_modified(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    (checkout / ".git").mkdir()
    (checkout / ".git/user-work").write_text("keep")
    lane = dev.compile_editable(cache, checkout)
    assert (checkout / ".git/user-work").read_text() == "keep"
    assert not any(name.startswith(".git") for name in lane["inputs"])
    assert not (Path(lane["workspace"]) / ".git").exists()


def test_dependency_drift_is_not_silently_accepted(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    (checkout / "package-lock.json").write_text("changed dependencies")
    with pytest.raises(build.BenchError, match="dependency lock changed"):
        dev.compile_editable(cache, checkout)


def test_incomplete_setup_requires_explicit_initialization(tmp_path, monkeypatch):
    cache, checkout, calls = editable(tmp_path, monkeypatch)
    marker = cache / "dev/setup.json"
    saved = json.loads(marker.read_text())
    saved["dependencies_ready"] = False
    build.write_json(marker, saved)
    with pytest.raises(build.BenchError, match="finish init"):
        dev.compile_editable(cache, checkout)
    assert dev.initialize(cache, checkout)["dependencies_ready"] is True
    assert any("ci" in call for call in calls)
    assert dev.compile_editable(cache, checkout)["cached"] is False


@pytest.mark.parametrize("name", ["helper.js", "src/helper.c", "src/helper.h"])
def test_unlisted_grammar_modules_and_native_sources_are_rejected(tmp_path, monkeypatch, name):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    (checkout / name).write_text("unsupported")
    with pytest.raises(build.BenchError, match=name):
        dev.compile_editable(cache, checkout)


def test_unrelated_repository_files_do_not_change_build_identity(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    first = dev.compile_editable(cache, checkout)
    (checkout / "README.md").write_text("documentation")
    (checkout / "lab").mkdir()
    (checkout / "lab/notes.py").write_text("tooling")
    assert dev.compile_editable(cache, checkout)["key"] == first["key"]


def test_generated_command_reports_and_refreshes_committed_generated_sources(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    lane = dev.compile_editable(cache, checkout)
    workspace = Path(lane["workspace"])
    assert dev.generated_drift(checkout, workspace) == list(dev.GENERATED)
    argv = ["cpp_dev.py", "generated", "--cache", str(cache), "--checkout", str(checkout)]
    monkeypatch.setattr("sys.argv", argv)
    assert dev.main() == 1
    monkeypatch.setattr("sys.argv", [*argv, "--write"])
    assert dev.main() == 0
    assert dev.generated_drift(checkout, workspace) == []
    monkeypatch.setattr("sys.argv", argv)
    assert dev.main() == 0


def test_sources_are_preserved_and_generated_outputs_stay_in_owned_workspace(tmp_path, monkeypatch):
    cache, checkout, calls = editable(tmp_path, monkeypatch)
    before = dev.source_inputs(checkout)
    lane = dev.compile_editable(cache, checkout)
    assert dev.source_inputs(checkout) == before
    assert not (checkout / "src/parser.c").exists()
    assert not (checkout / "node_modules").exists()
    assert Path(lane["workspace"]).is_relative_to(cache)
    assert not any(call[0] == "git" for call in calls)


@pytest.mark.parametrize("name", [
    "src/scanner.c", "src/scanner/new.h", "tree-sitter.json", "queries/tags.scm", "test/corpus/new.txt",
])
def test_every_handwritten_input_including_new_corpus_files_changes_build_identity(tmp_path, monkeypatch, name):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    first = dev.compile_editable(cache, checkout)
    path = checkout / name
    path.write_text(path.read_text() + "\n" if path.exists() else "new fixture")
    second = dev.compile_editable(cache, checkout)
    assert first["key"] != second["key"]
    assert second["inputs"][name] == build.digest(path)


def test_workspace_synchronization_removes_retired_inputs_and_recovers_tampering(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    path = checkout / "test/corpus/new.txt"
    path.write_text("temporary fixture")
    lane = dev.compile_editable(cache, checkout)
    workspace = Path(lane["workspace"])
    path.unlink()
    (workspace / "grammar.js").write_text("tampered")
    dev.compile_editable(cache, checkout)
    assert not (workspace / "test/corpus/new.txt").exists()
    assert (workspace / "grammar.js").read_bytes() == (checkout / "grammar.js").read_bytes()


def test_returning_to_cached_inputs_restores_matching_generated_native_sources(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    original = (checkout / "grammar.js").read_text()
    first = dev.compile_editable(cache, checkout)
    (checkout / "grammar.js").write_text("different grammar")
    dev.compile_editable(cache, checkout)
    (checkout / "grammar.js").write_text(original)
    restored = dev.compile_editable(cache, checkout)
    assert restored["cached"] is True
    assert restored["key"] == first["key"]
    assert build.source_hashes(Path(restored["workspace"])) == first["source_hashes"]
    assert json.loads((cache / "dev/build.json").read_text())["key"] == first["key"]


def test_workspace_owned_input_manifest_cannot_delete_outside_workspace(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    outside = cache / "dev/keep.txt"
    outside.write_text("keep")
    build.write_json(cache / "dev/workspace/inputs.json", {"../keep.txt": "tampered"})
    with pytest.raises(build.BenchError, match="Unsafe owned"):
        dev.compile_editable(cache, checkout)
    assert outside.read_text() == "keep"


def test_source_export_contains_new_corpus_and_inherited_inputs_not_generated_files(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    (checkout / "test/corpus/new.txt").write_text("new fixture")
    output = tmp_path / "sources.tar.gz"
    monkeypatch.setattr("sys.argv", ["cpp_dev.py", "export", "--cache", str(cache),
                                    "--checkout", str(checkout), "--output", str(output)])
    assert dev.main() == 0
    with tarfile.open(output) as archive:
        assert set(archive.getnames()) == dev.source_inputs(checkout).keys()
        assert "test/corpus/new.txt" in archive.getnames()


def test_native_corpus_preserves_deliberate_crlf_bytes_in_git():
    relative = "test/corpus/c/crlf.txt"
    source = build.ROOT / relative
    assert b"\r\n" in source.read_bytes()
    saved = subprocess.run(["git", "show", f":{relative}"], cwd=build.ROOT, check=True, capture_output=True)
    assert saved.stdout == source.read_bytes()


def test_unowned_workspace_and_legacy_receipts_are_preserved(tmp_path, monkeypatch):
    cache, checkout, _calls = editable(tmp_path, monkeypatch)
    marker = cache / "dev/setup.json"
    marker.unlink()
    workspace = cache / "dev/workspace"
    (workspace / "keep.txt").write_text("keep")
    with pytest.raises(build.BenchError, match="unowned"):
        dev.initialize(cache, checkout)
    assert (workspace / "keep.txt").read_text() == "keep"
    build.write_json(marker, {"checkout": str(checkout)})
    with pytest.raises(build.BenchError, match="legacy"):
        dev.initialize(cache, checkout)


def test_known_gap_is_visible_without_failing_the_regression_gate(monkeypatch):
    monkeypatch.setattr(bench, "fixtures", lambda: [{"id": "known-gap"}])
    report = {"cases": [{
        "id": "known-gap", "checks": {"baseline": {"passed": False}, "candidate": {"passed": False}},
    }]}
    verdict = dev.regression_verdict(report)
    assert verdict["regression_passed"] is True
    assert verdict["selected_cases_passed"] is False
    assert verdict["unresolved_cases"] == ["known-gap"]


@pytest.mark.parametrize("baseline,required", [(True, False), (False, True)])
def test_existing_correct_behavior_and_promoted_gaps_are_mandatory(monkeypatch, baseline, required):
    monkeypatch.setattr(bench, "fixtures", lambda: [{"id": "protected", "required": required}])
    report = {"cases": [{
        "id": "protected", "checks": {"baseline": {"passed": baseline}, "candidate": {"passed": False}},
    }]}
    verdict = dev.regression_verdict(report)
    assert verdict["regression_passed"] is False
    assert verdict["regression_failures"] == ["protected"]


@pytest.mark.parametrize("selection", [[], ["unknown"], ["product", "product"]])
def test_invalid_fixture_selection_fails_before_compiler_or_parser_execution(tmp_path, selection):
    with pytest.raises(build.BenchError, match="Case selection"):
        bench.compare(tmp_path, tmp_path / "report.json", case_ids=selection)
