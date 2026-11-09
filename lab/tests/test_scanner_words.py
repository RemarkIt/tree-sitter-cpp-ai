"""Scanner word lists must stay binary-searchable and match the grammar's keyword literals."""

import json
import re

import cpp_build as build

SCANNER = (build.ROOT / "src" / "scanner.c").read_text(encoding="utf-8")
LISTS = dict(re.findall(r"static const char \*const (\w+)\[\] = \{(.*?)\};", SCANNER, re.S))


def words(name: str) -> list[str]:
    return re.findall(r'"([^"]*)"', LISTS[name])


def test_word_lists_are_strictly_sorted_for_binary_search():
    assert {"KEYWORDS", "TYPE_KEYWORDS", "SPECIFIER_KEYWORDS"} <= LISTS.keys()
    for name in LISTS:
        listed = words(name)
        assert listed == sorted(set(listed)), f"{name} must be strictly ASCII-sorted without duplicates"


def test_keywords_are_exactly_the_identifier_shaped_grammar_literals():
    # The scanner returns any other word as an identifier, so a missing keyword would be mislexed.
    literals: set[str] = set()

    def collect(node: object) -> None:
        if isinstance(node, dict):
            if node.get("type") == "STRING":
                literals.add(node["value"])
            for value in node.values():
                collect(value)
        elif isinstance(node, list):
            for value in node:
                collect(value)

    collect(json.loads((build.ROOT / "src" / "grammar.json").read_text(encoding="utf-8"))["rules"])
    keywords = {literal for literal in literals if re.fullmatch(r"[A-Za-z_$][A-Za-z0-9_$]*", literal)}
    assert set(words("KEYWORDS")) == keywords
