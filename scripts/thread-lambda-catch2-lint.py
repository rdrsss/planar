#!/usr/bin/env python3
"""thread-lambda-catch2-lint.py — flag a Catch2 assertion macro reachable
from a worker-thread lambda in a `*.t.cpp` file (Planar plan 1069, task
6923).

Background (agentatomic.t.cpp, commit a59cb74f): Catch2's assertion macros
are NOT thread-safe -- they race on one global output-redirect flag inside
the Catch2 runtime. `make test-all`'s 16-thread claim test aborted
intermittently (~1 full suite run in 4) because its worker lambda called
`task_id_at(conn, i)`, which called `scalar_int`, which contains four
`REQUIRE`s -- sixteen threads entered those macros concurrently. The bug was
reached through TWO helper calls, not a macro written literally inside the
lambda, so a lint that only looks for `REQUIRE(` typed directly inside a
thread lambda would not have caught it.

What this script does:

  1. Within EACH file independently, finds every function-looking definition
     at brace depth 0 (a file-local helper -- exactly the shape of
     `exec`/`scalar_int`/`scalar_text`/`task_id_at` in agentatomic.t.cpp) and
     extracts its body.
  2. Marks a helper "unsafe" if its body contains a literal Catch2 assertion
     macro (`REQUIRE`, `CHECK`, `INFO`, `SECTION`, ...), then propagates that
     to a fixed point: a helper that calls another unsafe helper is unsafe
     too. This is the "reachable through helpers" part of the bug.
  3. Finds thread-launch sites: `std::vector<std::thread>` /
     `std::vector<std::jthread>` containers' `.emplace_back(...)` calls, and
     direct `std::thread(...)` / `std::jthread(...)` / `std::async(...)`
     constructions. Extracts the worker lambda (or bare callable) passed to
     each and scans it for a literal macro or a call to an unsafe helper.

What this does NOT catch (be honest about the gap rather than pretend
coverage): it is a single-file, brace-matching heuristic, not a real C++
parse. It will miss a helper defined in one file and called from a thread
lambda in ANOTHER file (Catch2 test helpers here are colocated per-file, so
this has not been observed, but it is a real gap); a helper invoked only
through a function pointer, `std::bind`, or a template indirection whose
callee name is not a plain identifier token; a thread launched through a
custom thread-pool abstraction that does not textually match
`std::thread`/`std::jthread`/`std::async`/a `std::vector<std::[j]thread>`
container; and a macro reached through more than one level of file-crossing
indirection. It intentionally still catches the ACTUAL historical bug
(`task_id_at` called from inside the worker lambda) because that reachability
is entirely within one file and matches the container-based launch pattern
this script recognizes -- see `scripts/test_thread_lambda_catch2_lint.py`
for a regression test that pins exactly this.

Usage:
    thread-lambda-catch2-lint.py [FILE ...]     lint given files
    thread-lambda-catch2-lint.py                lint every src/**/*.t.cpp

Exit 0 with nothing printed on a clean run; exit 1 and one line per
violation otherwise.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CATCH2_MACROS = {
    "REQUIRE",
    "REQUIRE_FALSE",
    "REQUIRE_THROWS",
    "REQUIRE_THROWS_AS",
    "REQUIRE_THROWS_WITH",
    "REQUIRE_THROWS_MATCHES",
    "REQUIRE_NOTHROW",
    "REQUIRE_THAT",
    "CHECK",
    "CHECK_FALSE",
    "CHECK_THROWS",
    "CHECK_THROWS_AS",
    "CHECK_THROWS_WITH",
    "CHECK_THROWS_MATCHES",
    "CHECK_NOTHROW",
    "CHECK_THAT",
    "INFO",
    "WARN",
    "FAIL",
    "FAIL_CHECK",
    "SUCCEED",
    "CAPTURE",
    "SECTION",
    "DYNAMIC_SECTION",
}

THREAD_CONTAINER_RE = re.compile(r"std::vector\s*<\s*std::j?thread\s*>\s+(\w+)")
FUNC_DEF_RE = re.compile(
    r"(?m)^[ \t]*(?:[\w:<>,&\*\s]+?)\b(\w+)\s*\(([^;{}]*)\)\s*"
    r"(?:->\s*[\w:<>,&\*\s]+)?\s*\{"
)
IDENTIFIER_RE = re.compile(r"\b([A-Za-z_]\w*)\s*(?:\(|\{)")


def mask(text: str) -> str:
    """Replace comment and string/char literal contents with `x` (same
    length, same newlines preserved) so brace matching and identifier
    scanning never trip on a brace or macro-shaped token inside a
    `std::format("...{}...")` literal or a comment.
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        two = text[i : i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j == -1 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = " "
            i = j
            continue
        ch = text[i]
        if ch in "\"'":
            quote = ch
            j = i + 1
            while j < n and text[j] != quote:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                j += 1
            j = min(j + 1, n)
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = "x"
            i = j
            continue
        i += 1
    return "".join(out)


def matching_brace(text: str, open_index: int) -> int:
    """Index of the `}` matching the `{` at `open_index`, or -1."""
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def matching_paren(text: str, open_index: int) -> int:
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


NON_FUNCTION_NAMES = {"if", "for", "while", "switch", "catch", "TEST_CASE", "SECTION", "DYNAMIC_SECTION"}


def find_file_local_functions(masked: str) -> dict[str, tuple[int, int]]:
    """Every named `name(...) { ... }` definition that is NOT nested inside
    a `TEST_CASE`/`SECTION` body, keyed by name -> (body start, body end)
    char offsets into `masked`.

    Deliberately NOT a depth-0-only scan: this file's real helpers
    (`exec`/`scalar_int`/`scalar_text`/`task_id_at` in agentatomic.t.cpp)
    sit inside an anonymous namespace, so they are at brace depth 1, not 0.
    What actually distinguishes "a file-local helper" from "a thing scanned
    FOR" is that it is not nested inside a test body -- a lambda or
    control-flow block inside a `TEST_CASE` is not itself a reusable
    helper. So this excludes `if`/`for`/`while`/`switch`/`catch` (which
    `FUNC_DEF_RE`'s parenthesized-condition shape can otherwise match) and
    anything whose definition starts inside a `TEST_CASE` span.
    """
    test_case_spans: list[tuple[int, int]] = []
    for match in FUNC_DEF_RE.finditer(masked):
        if match.group(1) != "TEST_CASE":
            continue
        open_brace = match.end() - 1
        close_brace = matching_brace(masked, open_brace)
        if close_brace != -1:
            test_case_spans.append((open_brace, close_brace))

    def inside_a_test_case(index: int) -> bool:
        return any(start <= index <= end for start, end in test_case_spans)

    functions: dict[str, tuple[int, int]] = {}
    for match in FUNC_DEF_RE.finditer(masked):
        name = match.group(1)
        if name in NON_FUNCTION_NAMES:
            continue
        if inside_a_test_case(match.start()):
            continue
        open_brace = match.end() - 1
        close_brace = matching_brace(masked, open_brace)
        if close_brace != -1:
            functions[name] = (open_brace, close_brace)
    return functions


def body_calls(masked: str, start: int, end: int) -> set[str]:
    return {m.group(1) for m in IDENTIFIER_RE.finditer(masked[start:end])}


def unsafe_helper_set(masked: str) -> dict[str, str]:
    """Name -> reason, for every file-local helper that is unsafe to call
    from a thread lambda: it either contains a Catch2 macro directly, or it
    (transitively, within this file) calls something that does.
    """
    functions = find_file_local_functions(masked)
    unsafe: dict[str, str] = {}
    for name, (start, end) in functions.items():
        calls = body_calls(masked, start, end)
        hit = calls & CATCH2_MACROS
        if hit:
            unsafe[name] = f"calls Catch2 macro {sorted(hit)[0]}(...) directly"
    changed = True
    while changed:
        changed = False
        for name, (start, end) in functions.items():
            if name in unsafe:
                continue
            calls = body_calls(masked, start, end)
            for callee in calls & set(unsafe):
                unsafe[name] = f"calls {callee}(...), which is unsafe: {unsafe[callee]}"
                changed = True
                break
    return unsafe


def find_lambda_span(masked: str, bracket_index: int) -> tuple[int, int] | None:
    """Given the index of a lambda's opening `[`, return (start, end) of
    its BODY (the `{...}` block), or None if this is not actually a lambda
    introducer (`[` not immediately followed by a capture-list shape).
    """
    close_bracket = masked.find("]", bracket_index)
    if close_bracket == -1:
        return None
    i = close_bracket + 1
    # Optional parameter list.
    while i < len(masked) and masked[i] in " \t\n":
        i += 1
    if i < len(masked) and masked[i] == "(":
        paren_close = matching_paren(masked, i)
        if paren_close == -1:
            return None
        i = paren_close + 1
    # Skip qualifiers / trailing return type up to the opening brace.
    brace_open = masked.find("{", i)
    if brace_open == -1:
        return None
    # Guard against skipping past an unrelated statement: nothing but
    # whitespace, `mutable`, `noexcept`, `->`, identifiers, and `<>,&*::`
    # should appear between here and the brace.
    between = masked[i:brace_open]
    if not re.fullmatch(r"[\sA-Za-z_0-9:<>,&\*\->]*", between):
        return None
    brace_close = matching_brace(masked, brace_open)
    if brace_close == -1:
        return None
    return brace_open, brace_close


THREAD_CTOR_RE = re.compile(r"\bstd::(?:thread|jthread|async)\s*[\(\{]")
EMPLACE_RE = re.compile(r"\b(\w+)\.emplace_back\s*\(")


def scan_file(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    masked = mask(raw)
    unsafe = unsafe_helper_set(masked)
    thread_containers = set(THREAD_CONTAINER_RE.findall(masked))
    violations: list[str] = []

    def check_launch_arg(arg_start: int, arg_end: int, site_line: int) -> None:
        segment = masked[arg_start:arg_end]
        bracket = segment.find("[")
        if bracket != -1 and bracket == len(segment) - len(segment.lstrip()):
            span = find_lambda_span(masked, arg_start + bracket)
            if span is not None:
                body_start, body_end = span
                calls = body_calls(masked, body_start, body_end)
                macro_hit = calls & CATCH2_MACROS
                if macro_hit:
                    violations.append(
                        f"{path}:{site_line}: thread lambda calls Catch2 macro "
                        f"{sorted(macro_hit)[0]}(...) directly"
                    )
                    return
                unsafe_hit = calls & set(unsafe)
                if unsafe_hit:
                    callee = sorted(unsafe_hit)[0]
                    violations.append(
                        f"{path}:{site_line}: thread lambda calls {callee}(...) -- "
                        f"{unsafe[callee]}"
                    )
                return
        # Not an inline lambda: a bare callable name passed directly.
        bare = segment.strip()
        if re.fullmatch(r"[A-Za-z_]\w*", bare) and bare in unsafe:
            violations.append(
                f"{path}:{site_line}: thread launched directly with "
                f"{bare} -- {unsafe[bare]}"
            )

    for match in EMPLACE_RE.finditer(masked):
        if match.group(1) not in thread_containers:
            continue
        open_paren = match.end() - 1
        close_paren = matching_paren(masked, open_paren)
        if close_paren == -1:
            continue
        check_launch_arg(open_paren + 1, close_paren, line_of(masked, match.start()))

    for match in THREAD_CTOR_RE.finditer(masked):
        opener = match.end() - 1
        closer = matching_paren(masked, opener) if masked[opener] == "(" else matching_brace(masked, opener)
        if closer == -1:
            continue
        check_launch_arg(opener + 1, closer, line_of(masked, match.start()))

    return violations


def main(argv: list[str]) -> int:
    if argv:
        files = [Path(arg) for arg in argv]
    else:
        files = sorted((ROOT / "src").rglob("*.t.cpp"))
    violations: list[str] = []
    for path in files:
        violations.extend(scan_file(path))
    if violations:
        for line in violations:
            print(line, file=sys.stderr)
        print(
            f"thread-lambda-catch2-lint: {len(violations)} violation(s)",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
