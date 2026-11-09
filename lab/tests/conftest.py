"""Run each lab test in an isolated working directory and retain failed cases on request."""

import hashlib
import os
import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    setattr(item, f"report_{report.when}", report)


@pytest.fixture(autouse=True)
def isolated_lab_test(request, monkeypatch, tmp_path_factory):
    case_root = tmp_path_factory.mktemp("case")
    workspace = case_root / "workspace"
    workspace.mkdir()
    monkeypatch.chdir(workspace)
    yield workspace
    call_report = getattr(request.node, "report_call", None)
    report = call_report if call_report is not None and call_report.failed else getattr(request.node, "report_setup", None)
    failure_root = os.environ.get("CPP_LAB_FAILURE_DIR")
    if report is not None and report.failed and failure_root:
        digest = hashlib.sha256(request.node.nodeid.encode("utf-8")).hexdigest()[:10]
        shutil.copytree(case_root, Path(failure_root) / f"{request.node.name}-{digest}", dirs_exist_ok=True)


@pytest.fixture
def tmp_path(isolated_lab_test):
    return isolated_lab_test
