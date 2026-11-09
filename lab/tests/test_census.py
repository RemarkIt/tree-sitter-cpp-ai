"""Private inputs must be exact, safe and reproducible."""

import hashlib

import pytest

import census
import cpp_build as build


def manifest(tmp_path, relative, data, sha=None):
    source = tmp_path / "source"
    source.mkdir()
    (source / "a.cpp").write_bytes(data)
    path = tmp_path / "manifest.json"
    build.write_json(path, {"files": [{"path": relative, "current_sha256": sha or hashlib.sha256(data).hexdigest()}]})
    return path


def test_manifest_verifies_exact_source_bytes(tmp_path):
    data = b"int f() { return 1; }\n"
    assert census.manifest_files(manifest(tmp_path, "a.cpp", data)) == [("a.cpp", data)]


def test_manifest_rejects_stale_source(tmp_path):
    with pytest.raises(build.BenchError, match="hash mismatch"):
        census.manifest_files(manifest(tmp_path, "a.cpp", b"int changed;", "stale"))


def test_manifest_rejects_duplicates(tmp_path):
    path = manifest(tmp_path, "a.cpp", b"int x;")
    entry = {"path": "a.cpp", "current_sha256": hashlib.sha256(b"int x;").hexdigest()}
    build.write_json(path, {"files": [entry, entry]})
    with pytest.raises(build.BenchError, match="duplicate"):
        census.manifest_files(path)


def test_manifest_rejects_outside_root(tmp_path):
    path = manifest(tmp_path, "../manifest.json", b"int x;")
    with pytest.raises(build.BenchError, match="Unsafe"):
        census.manifest_files(path)


def test_manifest_rejects_symlinks(tmp_path):
    path = manifest(tmp_path, "link.cpp", b"int x;")
    (tmp_path / "source/link.cpp").symlink_to(tmp_path / "source/a.cpp")
    with pytest.raises(build.BenchError, match="Unsafe"):
        census.manifest_files(path)


def test_manifest_rejects_empty_evidence(tmp_path):
    (tmp_path / "source").mkdir()
    path = tmp_path / "manifest.json"
    build.write_json(path, {"files": []})
    with pytest.raises(build.BenchError, match="empty"):
        census.manifest_files(path)


def test_manifest_never_shrinks_the_frozen_denominator(tmp_path):
    path = manifest(tmp_path, "a.cpp", b"int x;")
    with pytest.raises(build.BenchError, match="Expected 499 frozen files"):
        census.manifest_files(path, expected_files=499)


def test_projection_deltas_preserve_multiplicity_ownership_and_original_ranges():
    original = {"name": "f", "kind": "function", "owner": ["N"], "start": 0, "end": 10}
    changed = original | {"owner": [], "end": 11}
    delta = census.declaration_delta([original, original], [original, changed])
    assert delta == {"disappeared": [original], "appeared": [changed]}


def test_timing_summary_uses_medians_p95_and_independent_budgets():
    baseline = [{"parse_ns": {"a": 10, "b": 20}, "parse_projection_ns": 100, "incremental_ns": 20}] * 5
    candidate = [{"parse_ns": {"a": 12, "b": 24}, "parse_projection_ns": 120, "incremental_ns": 25}] * 5
    summary = census.timing_summary({"baseline": baseline, "candidate": candidate, "previous": baseline})
    assert summary["passed"] is True
    assert summary["metrics"]["baseline"]["parse_ns"] == 30
    assert summary["metrics"]["candidate"]["per_file_p95_ns"] == 24
    assert summary["comparisons"]["baseline"]["incremental_ns"]["ratio"] == 1.25
    candidate = candidate[:4] + [{"parse_ns": {"a": 1000, "b": 1000}, "parse_projection_ns": 1000, "incremental_ns": 1000}]
    assert census.timing_summary({"baseline": baseline, "candidate": candidate})["passed"] is False


@pytest.mark.parametrize("samples", [{}, {"baseline": []}, {
    "baseline": [{"parse_ns": {"a": 0}, "parse_projection_ns": 1, "incremental_ns": 1}] * 5,
    "candidate": [{"parse_ns": {"a": 1}, "parse_projection_ns": 1, "incremental_ns": 1}] * 5,
}])
def test_timing_rejects_missing_or_nonpositive_evidence(samples):
    with pytest.raises(build.BenchError, match="Timing"):
        census.timing_summary(samples)


def test_parse_work_budget_flags_unmerged_alternatives_per_case():
    baseline = {"ambiguous.cpp": 550, "plain.cpp": 540}
    merged = census.parse_work_summary({"baseline": baseline, "candidate": {"ambiguous.cpp": 556, "plain.cpp": 540}})
    assert merged["passed"] is True
    unmerged = census.parse_work_summary({"baseline": baseline, "candidate": {"ambiguous.cpp": 1083, "plain.cpp": 540}})
    assert unmerged["passed"] is False
    assert unmerged["cases"]["ambiguous.cpp"]["passed"] is False
    assert unmerged["cases"]["plain.cpp"]["passed"] is True


@pytest.mark.parametrize("work", [
    {"baseline": {"a.cpp": 1}}, {"baseline": {"a.cpp": 1}, "candidate": {"b.cpp": 1}},
    {"baseline": {"a.cpp": 0}, "candidate": {"a.cpp": 1}},
])
def test_parse_work_rejects_missing_or_nonpositive_evidence(work):
    with pytest.raises(build.BenchError, match="Parse work"):
        census.parse_work_summary(work)
