import hashlib
from itertools import count
import os
from pathlib import Path
import subprocess

import pytest

import census
import cpp_build as build


def test_syntax_census_counts_missing_punctuation_and_keeps_the_fixed_denominator(tmp_path):
    sources = {
        "clean.cpp": b"int add() { return 1; }\n", "missing.c": b"int value\n",
        "hidden.cpp": b'#include "x.h"',
    }
    directory = tmp_path / "source"
    directory.mkdir()
    for path, data in sources.items():
        (directory / path).write_bytes(data)
    manifest = tmp_path / "manifest.json"
    build.write_json(manifest, {"files": [
        {"path": path, "current_sha256": hashlib.sha256(data).hexdigest()} for path, data in sources.items()
    ]})
    baseline = Path(build.builds(build.DEFAULT_CACHE)["baseline"]["library"])
    report = census.measure(build.DEFAULT_CACHE, manifest, tmp_path / "census.json", baseline)
    assert report["candidate_origin"] == "external"
    assert report["syntax"]["baseline"] == report["syntax"]["candidate"]
    assert report["syntax"]["candidate"]["files"] == 3
    assert report["syntax"]["candidate"]["error_files"] == 2
    assert report["syntax"]["candidate"]["missing_nodes"] == 2
    missing_row = next(row for row in report["files"] if row["path"] == "missing.c")
    assert missing_row["diagnostics"]["candidate"] == [
        {"kind": ";", "start": 9, "end": 9, "missing": True},
    ]
    # The newline ending a directive is a hidden token that the node API never exposes.
    hidden_row = next(row for row in report["files"] if row["path"] == "hidden.cpp")
    assert hidden_row["diagnostics"]["candidate"] == [
        {"kind": "hidden", "start": 0, "end": 14, "missing": True, "within": "preproc_include"},
    ]
    assert report["syntax"]["candidate"]["maximum_error_files_for_10_percent"] == 0
    assert report["syntax_changes"] == {"newly_clean": [], "newly_broken": []}
    assert report["totals"]["disappeared"] == report["totals"]["appeared"] == 0


def test_prepared_raw_projection_and_timing_deny_processes_and_match_incremental_parsing(tmp_path, monkeypatch):
    sources = census.incremental_edits()
    directory = tmp_path / "source"
    directory.mkdir()
    for case in sources:
        (directory / case["path"]).write_bytes(case["before"])
    manifest = tmp_path / "manifest.json"
    build.write_json(manifest, {"files": [
        {"path": case["path"], "current_sha256": hashlib.sha256(case["before"]).hexdigest()} for case in sources
    ]})
    baseline = Path(build.builds(build.DEFAULT_CACHE)["baseline"]["library"])
    def denied(*_args, **_kwargs):
        pytest.fail("Prepared raw runtime attempted process execution")
    monkeypatch.setattr(subprocess, "Popen", denied)
    monkeypatch.setattr(os, "system", denied)
    monkeypatch.setattr(os, "posix_spawn", denied)
    monkeypatch.setattr(os, "fork", denied)
    monkeypatch.setattr(build, "run", denied)
    monkeypatch.setattr("platform.platform", denied)
    report = census.measure(build.DEFAULT_CACHE, manifest, tmp_path / "raw.json", baseline, baseline, len(sources))
    for comparison in report["comparisons"].values():
        assert all(not value for value in comparison.values())
    ticks = count(0, 100)
    timing = census.timing(build.DEFAULT_CACHE, manifest, tmp_path / "timing.json", baseline, baseline, len(sources),
                           clock=lambda: next(ticks))
    assert timing["summary"]["passed"] is True
    assert all(len(runs) == 5 for runs in timing["samples"].values())
    assert timing["protocol"]["orders"][:2] == [
        ["baseline", "candidate", "previous"], ["previous", "candidate", "baseline"],
    ]
    assert all(case["matched"] for cases in timing["incremental"].values() for case in cases)
    work = timing["summary"]["parse_work"]
    assert work["passed"] is True
    assert all(steps > 0 for lane in work["steps"].values() for steps in lane.values())
    assert work["steps"]["baseline"] == work["steps"]["candidate"]


def test_census_strict_ten_percent_threshold_and_previous_projection(tmp_path):
    directory = tmp_path / "source"
    directory.mkdir()
    data = b"namespace N { int f(); int x; }\n"
    for index in range(10):
        (directory / f"{index}.cpp").write_bytes(data)
    manifest = tmp_path / "manifest.json"
    build.write_json(manifest, {"files": [
        {"path": f"{index}.cpp", "current_sha256": hashlib.sha256(data).hexdigest()} for index in range(10)
    ]})
    baseline = Path(build.builds(build.DEFAULT_CACHE)["baseline"]["library"])
    report = census.measure(build.DEFAULT_CACHE, manifest, tmp_path / "raw.json", baseline, baseline, 10)
    assert report["syntax"]["candidate"]["maximum_error_files_for_10_percent"] == 0
    projection = report["files"][0]["projection"]["candidate"]
    assert {(value["name"], tuple(value["owner"]), value["kind"]) for value in projection} == {
        ("f", ("N",), "function"), ("x", ("N",), "variable"),
    }
    assert report["comparisons"]["previous"]["declarations"] == []
