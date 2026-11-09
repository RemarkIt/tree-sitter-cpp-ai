import argparse
import json
from pathlib import Path

import pytest

import census
import cpp_build as build
import bench
import tools


@pytest.fixture(scope="module")
def upstream():
    return bench.parser_for(Path(build.builds(build.DEFAULT_CACHE)["baseline"]["library"]))


SOURCE = b"""#include <secret/path.h>
#include "local/secret.h"
// Proprietary note about CustomerAccount
#define ACCOUNT_API(x) __declspec(dllexport) x
namespace Billing {
struct CustomerAccount {
    int balance = 0x1F;
    const char* label = "acct\\n\\x41";
    char quote = '\\'';
    const char* raw = R"tag(raw "text")tag";
    /* block comment with Billing */
    int Charge(int cents) { return balance -= cents * 1'000; }
};
}
"""


def test_anonymize_preserves_syntax_and_renames_consistently(upstream):
    result = tools.anonymize(SOURCE)
    for secret in (b"secret", b"Billing", b"CustomerAccount", b"Proprietary", b"acct", b"balance", b"Charge"):
        assert secret not in result
    for kept in (b"#include <h.h>", b'#include "h.h"', b"namespace", b"struct", b"__declspec", b"0x1F", b"1'000",
                 b"R\"tag(", b")tag\""):
        assert kept in result
    assert b"namespace T1 {" in result and result.count(b"T1") == 1
    assert str(upstream.parse(result).root_node) == str(upstream.parse(SOURCE).root_node)


def test_anonymize_keeps_requested_names():
    assert tools.anonymize(b"int Keep(int drop);", frozenset({"Keep"})) == b"int Keep(int n1);"


def test_reduction_keeps_only_lines_needed_for_the_property():
    lines = [f"line{index}\n".encode() for index in range(20)]
    reduced = tools.reduce_lines(lines, lambda data: b"line3\n" in data and b"line17\n" in data)
    assert reduced == [b"line3\n", b"line17\n"]
    with pytest.raises(build.BenchError, match="does not have the requested property"):
        tools.reduce_lines(lines, lambda data: False)


def test_glr_activity_matches_the_parse_work_counter(upstream):
    for case in census.parse_work_cases():
        activity = tools.parse_versions(upstream, case["source"])
        assert activity["process_events"] == census.parse_steps(upstream, case["source"]) > 0
    ambiguous = tools.parse_versions(upstream, census.parse_work_cases()[0]["source"])
    assert ambiguous["max_versions"] >= 2


def test_tree_lines_label_fields_only_on_their_own_node(upstream):
    lines = tools.tree_lines(upstream, b"int f(int x);\n")
    assert sum("declarator: function_declarator" in line for line in lines) == 1
    assert any(line.strip().endswith("parameters: parameter_list") for line in lines)
    assert all(line[:5].strip() == "1" for line in lines)


def test_compare_reports_identity_errors_and_declaration_deltas(tmp_path, upstream, capsys):
    (tmp_path / "source").mkdir()
    files = {"same.cpp": b"int f();\n", "broken.cpp": b"int g( {\n"}
    for name, data in files.items():
        (tmp_path / "source" / name).write_bytes(data)
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"files": [
        {"path": name, "current_sha256": build.digest(tmp_path / "source" / name)} for name in files
    ]}))
    args = argparse.Namespace(old="upstream", new="upstream", manifest=manifest, output=None,
                              cache=build.DEFAULT_CACHE)
    assert tools.command_compare(args) == 0
    report = json.loads(capsys.readouterr().out)
    assert report["summary"] == {"identical": 2, "changed": 0, "fixed": 0, "broken": 0,
                                 "first_error_moved": 0, "declarations": 0}
