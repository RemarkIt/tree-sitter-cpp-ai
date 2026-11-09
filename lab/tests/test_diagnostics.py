from dataclasses import replace

import bench


def test_type_and_source_boundary_changes_are_explicit():
    expected = bench.Symbol("a.cpp", "f", "function", (), 0, 20, True, "int")
    actual = replace(expected, start=4, return_type="float")
    delta = bench.difference([expected], [actual])
    assert delta["changed"][0]["fields"] == ["start", "return_type"]
    assert delta["missing"] == [bench.asdict(expected)]
    assert delta["unexpected"] == [bench.asdict(actual)]


def test_same_name_different_ownership_is_not_a_false_match():
    expected = bench.Symbol("a.cpp", "f", "function", ("N",), 0, 20, True, "int")
    actual = replace(expected, owner=("M",))
    delta = bench.difference([expected], [actual])
    assert delta["changed"] == []
    assert delta["missing"] and delta["unexpected"]
