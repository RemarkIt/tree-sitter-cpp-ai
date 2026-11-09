"""Investigation tools: inspect, compare, reduce, anonymize and profile parse trees.

Libraries are paths to compiled parsers or the aliases `upstream` (prepared baseline lane)
and `dev` (latest editable build). Manifests are census manifests (see corpus.py).
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import statistics
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any, Callable, Iterator

from tree_sitter import Node, Parser

import bench
import census
import cpp_build as build


def has_error(root: Node) -> bool:
    return root.has_error


def first_error(root: Node) -> tuple[int, str] | None:
    for node in bench.walk(root):
        if node.type == "ERROR" or node.is_missing:
            return node.start_point[0] + 1, node.type
    if root.has_error:
        return root.start_point[0] + 1, "hidden"
    return None


def sexp(node: Node, data: bytes, depth: int = 0) -> Iterator[str]:
    """Indented tree lines annotated with 1-based source lines."""
    field_names = {}
    for index, child in enumerate(node.children):
        name = node.field_name_for_child(index)
        if name:
            field_names[child.id] = name
    label = node.type if not node.is_missing else f"MISSING {node.type}"
    leaf = "" if node.named_child_count else f"  {data[node.start_byte:node.end_byte].decode('utf-8', 'replace')!r}"
    yield f"{node.start_point[0] + 1:>5}  {'  ' * depth}{label}{leaf}"
    for child in node.children:
        if child.is_named or child.is_missing:
            name = field_names.get(child.id)
            lines = sexp(child, data, depth + 1)
            first = next(lines)
            yield first if not name else first.replace(child.type, f"{name}: {child.type}", 1)
            yield from lines


def tree_lines(parser: Parser, data: bytes, first: int | None = None, last: int | None = None) -> list[str]:
    root = parser.parse(data).root_node
    lines = list(sexp(root, data))
    if first is None:
        return lines
    return [line for line in lines if first <= int(line[:5]) <= (last or first)]


def command_tree(args: argparse.Namespace) -> int:
    parser = bench.parser_for(build.resolve_library(args.library, args.cache))
    first, _, last = (args.lines or "").partition(":")
    for line in tree_lines(parser, args.file.read_bytes(), int(first) if first else None,
                           int(last) if last else None):
        print(line)
    return 0


def command_diff(args: argparse.Namespace) -> int:
    parsers = [bench.parser_for(build.resolve_library(value, args.cache)) for value in (args.old, args.new)]
    changed = 0
    for path in args.files:
        data = path.read_bytes()
        old, new = (tree_lines(parser, data) for parser in parsers)
        if old == new:
            continue
        changed += 1
        print(f"=== {path}")
        diff = list(difflib.unified_diff(old, new, "old", "new", n=args.context, lineterm=""))
        print("\n".join(diff[2:2 + args.limit]))
    return 1 if changed else 0


def manifest_trees(parser: Parser, manifest: Path) -> dict[str, tuple[str, Node, bytes]]:
    result = {}
    for relative, data in census.manifest_files(manifest):
        root = parser.parse(data).root_node
        result[relative] = (str(root), root, data)
    return result


def command_compare(args: argparse.Namespace) -> int:
    """Tree identity, error-status changes and declaration deltas between two parsers on a corpus."""
    old_parser, new_parser = (bench.parser_for(build.resolve_library(value, args.cache)) for value in (args.old, args.new))
    report: dict[str, Any] = {"identical": 0, "changed": [], "fixed": [], "broken": [], "first_error_moved": [],
                              "declarations": {}}
    for relative, data in census.manifest_files(args.manifest):
        old_root, new_root = old_parser.parse(data).root_node, new_parser.parse(data).root_node
        if str(old_root) == str(new_root):
            report["identical"] += 1
            continue
        report["changed"].append(relative)
        if old_root.has_error and not new_root.has_error:
            report["fixed"].append(relative)
        elif new_root.has_error and not old_root.has_error:
            report["broken"].append(relative)
        elif first_error(old_root) != first_error(new_root):
            report["first_error_moved"].append({"path": relative, "old": first_error(old_root),
                                                "new": first_error(new_root)})
        identities = [Counter((symbol.name, symbol.kind, symbol.owner) for symbol in bench.tree_symbols(relative, data, root))
                      for root in (old_root, new_root)]
        lost, gained = identities[0] - identities[1], identities[1] - identities[0]
        if lost or gained:
            report["declarations"][relative] = {
                "lost": sorted(f"{kind} {'::'.join((*owner, name))}" for name, kind, owner in lost.elements()),
                "gained": sorted(f"{kind} {'::'.join((*owner, name))}" for name, kind, owner in gained.elements()),
            }
    report["summary"] = {key: report[key] if key == "identical" else len(report[key])
                         for key in ("identical", "changed", "fixed", "broken", "first_error_moved", "declarations")}
    if args.output:
        build.write_json(args.output, report)
    print(json.dumps(report["summary"] if args.output else report, indent=None if args.output else 2))
    return 0


def reduce_lines(lines: list[bytes], interesting: Callable[[bytes], bool]) -> list[bytes]:
    """Delta debugging over lines: the smallest found subset that keeps `interesting` true."""
    if not interesting(b"".join(lines)):
        raise build.BenchError("The input does not have the requested property.")
    granularity = 2
    while len(lines) >= 2:
        size = max(1, len(lines) // granularity)
        reduced = False
        for start in range(0, len(lines), size):
            complement = lines[:start] + lines[start + size:]
            if complement and interesting(b"".join(complement)):
                lines, granularity, reduced = complement, max(granularity - 1, 2), True
                break
        if not reduced:
            if size == 1:
                break
            granularity = min(granularity * 2, len(lines))
    return lines


def property_for(args: argparse.Namespace) -> Callable[[bytes], bool]:
    parser = bench.parser_for(build.resolve_library(args.library, args.cache))
    if args.against:
        other = bench.parser_for(build.resolve_library(args.against, args.cache))
        if args.property == "error":
            return lambda data: parser.parse(data).root_node.has_error and not other.parse(data).root_node.has_error
        return lambda data: str(parser.parse(data).root_node) != str(other.parse(data).root_node)
    if args.property == "differs":
        raise build.BenchError("--property differs requires --against")
    return lambda data: parser.parse(data).root_node.has_error


def command_reduce(args: argparse.Namespace) -> int:
    interesting = property_for(args)
    lines = reduce_lines(args.file.read_bytes().splitlines(keepends=True), interesting)
    result = b"".join(lines)
    (args.output.write_bytes(result) if args.output else sys.stdout.buffer.write(result))
    print(f"reduced to {len(lines)} lines", file=sys.stderr)
    return 0


KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class
co_await co_return co_yield compl concept const consteval constexpr constinit const_cast continue decltype
default delete do double dynamic_cast else enum explicit export extern false final float for friend goto if
import inline int long module mutable namespace new noexcept not not_eq nullptr operator or or_eq override
private protected public register reinterpret_cast requires return short signed sizeof static static_assert
static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned
using virtual void volatile wchar_t while xor xor_eq _Alignas _Alignof _Atomic _Bool _Complex _Generic
_Imaginary _Noreturn _Static_assert _Thread_local restrict __attribute__ __attribute __declspec __cdecl
__stdcall __fastcall __thiscall __vectorcall __restrict __restrict__ __inline __inline__ __forceinline
__extension__ __asm__ __asm __volatile__ __based __unaligned __ptr32 __ptr64 __int8 __int16 __int32 __int64
__try __except __finally __leave __has_include __has_cpp_attribute __VA_ARGS__ __VA_OPT__ defined
include define undef ifdef ifndef elif elifdef elifndef endif error warning pragma line
size_t ptrdiff_t nullptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t
std
""".split())
TOKEN = re.compile(rb"""
    (?P<include>^[ \t]*\#[ \t]*(?:include_next|include|import)[ \t]*)(?P<header><[^>\n]*>|"[^"\n]*")
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^()\\ \t\n]{0,16})\(.*?\)(?P=delimiter)")
  | (?P<string>(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")
  | (?P<char>(?:u8|u|U|L)?'(?:\\.|[^'\\\n])*')
  | (?P<number>\.?\d(?:[\w.']|[eEpP][+-])*)
  | (?P<identifier>[A-Za-z_]\w*)
""", re.VERBOSE | re.DOTALL | re.MULTILINE)
ESCAPE_OR_TEXT = re.compile(rb"\\(?:\r?\n|x[0-9a-fA-F]+|u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[0-7]{1,3}|.)|\S", re.DOTALL)


def blank_text(body: bytes) -> bytes:
    """Replace literal text with `x`, every escape with `\\n`; keep spacing and line continuations."""
    def substitute(match: re.Match[bytes]) -> bytes:
        token = match.group(0)
        if token.startswith(b"\\"):
            return token if token[1:] in (b"\n", b"\r\n") else b"\\n"
        return b"x"
    return ESCAPE_OR_TEXT.sub(substitute, body)


def anonymize(data: bytes, keep: frozenset[str] = frozenset()) -> bytes:
    """Rename identifiers consistently and blank literal and comment text, keeping every token's syntax."""
    kept = KEYWORDS | keep
    names: dict[bytes, bytes] = {}
    counters = Counter[str]()

    def rename(word: bytes) -> bytes:
        if word.decode() in kept:
            return word
        if word not in names:
            # Macro-style spelling stays recognizable to readers; the grammar never relies on it.
            prefix = "M" if word.isupper() and len(word) > 1 else "T" if word[:1].isupper() else "n"
            counters[prefix] += 1
            names[word] = f"{prefix}{counters[prefix]}".encode()
        return names[word]

    def replace(match: re.Match[bytes]) -> bytes:
        kind = match.lastgroup
        token = match.group(0)
        if kind == "header":
            return match.group("include") + (b"<h.h>" if match.group("header").startswith(b"<") else b'"h.h"')
        if kind == "comment":
            return re.sub(rb"[^\s/*\\]", b"x", token)
        if kind == "raw":
            head_length = token.index(b'"') + len(match.group("delimiter")) + 2
            tail_length = len(match.group("delimiter")) + 2
            body = token[head_length:-tail_length]
            return token[:head_length] + re.sub(rb"\S", b"x", body) + token[-tail_length:]
        if kind in {"string", "char"}:
            start = token.index(b'"' if kind == "string" else b"'") + 1
            return token[:start] + blank_text(token[start:-1]) + token[-1:]
        if kind == "identifier":
            return rename(token)
        return token

    return TOKEN.sub(replace, data)


def command_anonymize(args: argparse.Namespace) -> int:
    result = anonymize(args.file.read_bytes(), frozenset(args.keep or ()))
    if args.library:
        parser = bench.parser_for(build.resolve_library(args.library, args.cache))
        before = parser.parse(args.file.read_bytes()).root_node.has_error
        after = parser.parse(result).root_node.has_error
        print(json.dumps({"error_before": before, "error_after": after}), file=sys.stderr)
    (args.output.write_bytes(result) if args.output else sys.stdout.buffer.write(result))
    return 0


def parse_versions(parser: Any, data: bytes) -> dict[str, Any]:
    """GLR activity: processed versions overall and the widest row, from the parser's debug log."""
    events = 0
    rows: Counter[int] = Counter()
    pattern = re.compile(r"process version:(\d+), version_count:(\d+), state:\d+, row:(\d+)")

    def log(_kind: Any, message: str) -> None:
        nonlocal events
        match = pattern.match(message)
        if match:
            events += 1
            rows[int(match.group(3))] = max(rows[int(match.group(3))], int(match.group(2)))

    parser.logger = log
    try:
        parser.parse(data)
    finally:
        parser.logger = None
    widest = max(rows.items(), key=lambda item: (item[1], -item[0]), default=(0, 0))
    return {"process_events": events, "max_versions": widest[1], "first_widest_row": widest[0] + 1,
            "rows_with_multiple_versions": sum(1 for count in rows.values() if count > 1)}


def command_glr(args: argparse.Namespace) -> int:
    rows = {}
    for value in args.libraries:
        parser = bench.parser_for(build.resolve_library(value, args.cache))
        rows[value] = parse_versions(parser, args.file.read_bytes())
    print(json.dumps(rows, indent=2))
    return 0


def command_time(args: argparse.Namespace) -> int:
    """Per-file median parse time ratio between two parsers; ranks the slowest files."""
    parsers = [bench.parser_for(build.resolve_library(value, args.cache)) for value in (args.old, args.new)]
    rows: list[dict[str, Any]] = []
    for relative, data in census.manifest_files(args.manifest):
        samples: list[list[float]] = [[], []]
        for _ in range(args.repeat):
            for index, parser in enumerate(parsers):
                start = time.perf_counter()
                parser.parse(data)
                samples[index].append(time.perf_counter() - start)
        old, new = (statistics.median(values) for values in samples)
        rows.append({"path": relative, "bytes": len(data), "old_ms": old * 1e3, "new_ms": new * 1e3,
                     "ratio": new / old if old else float("inf")})
    ratios = [row["ratio"] for row in rows]
    total = {"files": len(rows), "median_ratio": statistics.median(ratios),
             "total_ratio": sum(row["new_ms"] for row in rows) / sum(row["old_ms"] for row in rows)}
    print(json.dumps({"summary": total, "slowest": sorted(rows, key=lambda row: -row["ratio"])[:args.top]},
                     indent=2))
    return 0


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--cache", type=Path, default=build.DEFAULT_CACHE)
    commands = cli.add_subparsers(dest="command", required=True)
    tree = commands.add_parser("tree", help="Print a parse tree, optionally for a 1-based line range A:B.")
    tree.add_argument("library")
    tree.add_argument("file", type=Path)
    tree.add_argument("--lines")
    tree.set_defaults(handler=command_tree)
    diff = commands.add_parser("diff", help="Show where two parsers' trees differ for files.")
    diff.add_argument("old")
    diff.add_argument("new")
    diff.add_argument("files", type=Path, nargs="+")
    diff.add_argument("--context", type=int, default=3)
    diff.add_argument("--limit", type=int, default=80, help="maximum diff lines per file")
    diff.set_defaults(handler=command_diff)
    compare = commands.add_parser("compare", help="Tree identity, error changes and declaration deltas on a corpus.")
    compare.add_argument("old")
    compare.add_argument("new")
    compare.add_argument("--manifest", type=Path, required=True)
    compare.add_argument("--output", type=Path)
    compare.set_defaults(handler=command_compare)
    reduce = commands.add_parser("reduce", help="Delta-debug a file to the lines that keep a property.")
    reduce.add_argument("library")
    reduce.add_argument("file", type=Path)
    reduce.add_argument("--property", choices=("error", "differs"), default="error",
                        help="error: the tree has errors (and --against parses cleanly); differs: trees differ")
    reduce.add_argument("--against", help="second parser for a differential property")
    reduce.add_argument("--output", type=Path)
    reduce.set_defaults(handler=command_reduce)
    anon = commands.add_parser("anonymize", help="Rename identifiers and blank literal/comment text.")
    anon.add_argument("file", type=Path)
    anon.add_argument("--keep", action="append", help="identifier to keep verbatim (repeatable)")
    anon.add_argument("--library", help="report whether the parse still has errors")
    anon.add_argument("--output", type=Path)
    anon.set_defaults(handler=command_anonymize)
    glr = commands.add_parser("glr", help="GLR version activity per parser for one file.")
    glr.add_argument("file", type=Path)
    glr.add_argument("libraries", nargs="+")
    glr.set_defaults(handler=command_glr)
    timing = commands.add_parser("time", help="Rank files by parse-time ratio between two parsers.")
    timing.add_argument("old")
    timing.add_argument("new")
    timing.add_argument("--manifest", type=Path, required=True)
    timing.add_argument("--repeat", type=int, default=5)
    timing.add_argument("--top", type=int, default=20)
    timing.set_defaults(handler=command_time)
    args = cli.parse_args()
    try:
        args.cache = args.cache.resolve()
        return int(args.handler(args))
    except (build.BenchError, OSError, ValueError, KeyError) as error:
        print(f"cpp-parser-lab tools: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
