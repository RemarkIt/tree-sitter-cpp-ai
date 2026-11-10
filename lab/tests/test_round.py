"""The round verdict must separate hard failures, decisions and real improvements."""

import round as rounds


def symbol(name, start, end, kind="function"):
    return {"name": name, "kind": kind, "start": start, "end": end}


def corpus(previous=(2, 10, 100), candidate=(1, 5, 50), **changes):
    keys = ("files", "sites", "bytes")
    return {"previous": dict(zip(keys, previous)), "candidate": dict(zip(keys, candidate)),
            "newly_broken": [], "newly_clean": [], "changed_trees": 1, "clean_changed": [],
            "lost": [], "gained": [], "junk_lost": 0, **changes}


def passing(**corpora):
    return {"native": {"passed": True}, "labtests": {"passed": True}, "gate": {"passed": True}, "corpora": corpora}


def test_moved_or_reshaped_declarations_are_not_losses():
    old = [symbol("f", 0, 10), symbol("g", 20, 30), symbol("h", 40, 50)]
    new = [symbol("f", 0, 10), symbol("g", 22, 35), symbol("h", 45, 48, kind="variable")]
    assert rounds.unmatched_declarations(old, new) == []


def test_missing_or_disjoint_declarations_are_losses():
    old = [symbol("f", 0, 10), symbol("f", 20, 30), symbol("g", 40, 50)]
    new = [symbol("f", 0, 10), symbol("g", 60, 70)]
    assert rounds.unmatched_declarations(old, new) == [symbol("f", 20, 30), symbol("g", 40, 50)]


def test_corpus_summary_filters_junk_names_and_flags_clean_changes():
    def entry(path, errors, trees, previous, candidate):
        return {"path": path, "syntax_error": {"previous": errors[0], "candidate": errors[1]},
                "tree_sha256": {"previous": trees[0], "candidate": trees[1]},
                "diagnostics": {"previous": [{}] * errors[0], "candidate": [{}] * errors[1]},
                "projection": {"previous": previous, "candidate": candidate}}

    report = {
        "syntax": {lane: {"error_files": 1, "error_region_bytes": 5} for lane in ("previous", "candidate")},
        "comparisons": {"previous": {"newly_broken": [], "newly_clean": [], "changed_trees": ["a.cpp"]}},
        "files": [
            entry("a.cpp", (False, False), ("x", "y"), [symbol("MACRO_NAME", 0, 5), symbol("real", 6, 9)], []),
            entry("b.cpp", (True, True), ("z", "z"), [], [symbol("added", 0, 3)]),
        ],
    }
    summary = rounds.corpus_summary(report)
    assert summary["clean_changed"] == ["a.cpp"]
    assert [row["name"] for row in summary["lost"]] == ["real"]
    assert summary["junk_lost"] == 1
    assert [row["name"] for row in summary["gained"]] == ["added"]
    assert summary["candidate"] == {"files": 1, "sites": 1, "bytes": 5}


def test_verdict_accepts_only_clean_improvements():
    assert rounds.verdict(passing(heldout=corpus())) == ("ACCEPT", [])


def test_verdict_fails_on_hard_checks_before_review_items():
    results = passing(heldout=corpus(newly_broken=["a.cpp"], lost=[{}]))
    results["gate"]["passed"] = False
    status, reasons = rounds.verdict(results)
    assert status == "FAIL"
    assert reasons[:2] == ["gate failed", "heldout: 1 newly broken"]
    assert "heldout: 1 declarations lost" in reasons


def test_verdict_asks_for_review_without_improvement_or_with_mixed_counts():
    assert rounds.verdict(passing(heldout=corpus(candidate=(2, 10, 100)))) == ("REVIEW", ["no corpus improvement"])
    status, reasons = rounds.verdict(passing(heldout=corpus(candidate=(1, 12, 50))))
    assert (status, reasons) == ("REVIEW", ["heldout: sites increased"])
    results = passing(heldout=corpus())
    results["timing"] = {"passed": False}
    assert rounds.verdict(results)[0] == "FAIL"
