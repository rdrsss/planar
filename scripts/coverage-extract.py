#!/usr/bin/env python3
"""coverage-extract.py — extract (verb, subcommand) leaves exercised by the
C++ Catch2 test corpus, for scripts/coverage-check.sh.

Replaces the Zig-era `&.{ "verb", "sub" }` slice-literal scan (task 6436,
decision 1035): the corpus this gate measures moved from
`zig/integration_tests/*.zig` to the C++ `*.t.cpp` files under `src/`, and
the extraction shape changed with it. This file matches CALL SHAPE, never
string adjacency — see "Trap 2" below for why that distinction is
load-bearing.

## What counts as "exercised"

Only two call shapes count:

  1. `dispatch(fx, {"verb", "sub", ...})` — the in-process Catch2 leaf
     tests (`src/cmd/*/*_leaves.t.cpp` and friends). `dispatch` is a
     locally-defined helper with signature `dispatch(const fixture&,
     std::vector<std::string>)` — every file that defines it takes the
     same shape, so matching the call site (not the definition) is
     sufficient and portable across files.
  2. `run_pinned(cpp_bin(), <arg-list>, ...)` — the cross-process
     black-box tests (`uncovered_leaves.t.cpp`, `cross_process.t.cpp`,
     `parity.t.cpp`, `statediff.t.cpp`). Only `cpp_bin()` invocations
     count: it always resolves to the `planar` binary (see each file's
     local `auto cpp_bin() -> std::filesystem::path { return
     std::filesystem::path{PLANAR_CPP_BIN}; }`), which is the binary this
     gate's universe is derived from. `run_pinned` calls against
     `ext_bin`/`agent_bin`/`watch_bin` etc. exercise a DIFFERENT binary's
     verb surface and are out of scope for this gate.

`<arg-list>` can be:
  - an inline brace-init list: `{"verb", "sub", ...}`
  - `std::array<std::string, N>{"verb", "sub", ...}`
  - `std::vector<std::string>{"verb", "sub", ...}`
  - a `IDENT.args` / `IDENT.argv` field reference into a table-driven
    loop variable (see "Trap 1" below)

## Trap 1 — table-driven loops are real coverage, not blind spots we can
## just accept

The Zig gate's header documented (and left unhandled) exactly this blind
spot: `scope_leaves.t.cpp:392`'s

    for (auto const& verb : {"use", "pop", "clear"}) {
      dispatch(fx, {"scope", std::string{verb}});
    }

genuinely dispatches `scope use`, `scope pop`, and `scope clear`, but a
naive scanner sees only `{"scope", <not a string literal>}` and
undercounts by two leaves. This extractor resolves it: when it finds a
range-for loop `for (auto const& VAR : {"lit1", "lit2", ...})`, it
inlines each literal for every occurrence of `VAR` (bare, or wrapped as
`std::string{VAR}`) inside a `dispatch`/`run_pinned` arg-list textually
contained in that loop's body.

The much more common table-driven shape in the C++ corpus is a
`std::vector<row> const rows{ {"tag", {"verb", "sub", ...}, code, out,
err}, ... };` literal, iterated as `for (auto const& row : rows)
{ run_pinned(cpp_bin(), row.args, ...); }` (see `parity.t.cpp`'s `leaf`/
`step` tables). This extractor resolves that shape too: given a
`IDENT.args`/`IDENT.argv` field reference inside `for (... IDENT :
CONTAINER)`, it locates CONTAINER's brace-init definition, splits it into
per-row top-level groups, and within each row takes the FIRST nested
brace group whose elements are ALL plain string literals (2+ of them) as
that row's arg list — which is exactly the field CLAUDE.md's row structs
call `args`/`argv`, positionally, without this script needing to parse
the struct definition to know the field's index.

Anything else — an arg list built by concatenation, a runtime-computed
positional, a helper function's return value — is a genuine remaining
blind spot. It is undercounting, never overcounting: a leaf can only be
missed here, never fabricated.

## Trap 2 — a leaf NAME is not coverage

`parity.t.cpp` pins the whole `planar schema` JSON catalog as ONE massive
raw-string literal (`R"CATALOG6542(...)CATALOG6542"`), which contains
every verb and subcommand name as JSON *substrings* — `"artifact"` next
to `"edit"` next to `"review"`, etc. A scanner that tokenizes by naive
`"..."` regex without understanding raw-string delimiters would explode
that one token into hundreds of adjacent-looking fake "string literals"
and misread the catalog dump as covering nearly everything.

This extractor tokenizes C++ raw strings (`R"delim(...)delim"`) as a
SINGLE opaque token before any quote-scanning happens, so the catalog
body is invisible to the string-literal matcher — it is never split into
fragments in the first place. Ordinary comments (`//...`, `/*...*/`) are
also stripped before tokenizing, so the prose transcripts in file-header
comments (e.g. artifact_leaves.t.cpp's `$Z artifact list ...` examples)
never leak into the token stream either.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass


# ---------- tokenizer ----------
#
# Token kinds: STR (a resolved string literal, raw or ordinary — .value
# holds the decoded text), IDENT, LBRACE, RBRACE, COMMA, OTHER (a single
# other character, kept only so brace/paren matching elsewhere in the
# file stays positionally sane; content is irrelevant to this script).

@dataclass
class Tok:
    kind: str
    value: str
    pos: int  # character offset of token start in the ORIGINAL source


RAW_STRING_RE = re.compile(r'R"([^ ()\\\t]{0,16})\((.*?)\)\1"', re.DOTALL)
ORD_STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"')
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
IDENT_RE = re.compile(r"[A-Za-z_]\w*")


def strip_comments(src: str) -> str:
    """Blank out comments, preserving newlines/length so offsets stay valid.

    A `//` or `/*` inside a raw or ordinary string literal must NOT be
    treated as a comment start, so raw/ordinary strings are recognized and
    copied through verbatim before comment-detection ever runs on that
    span.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        m = RAW_STRING_RE.match(src, i)
        if m:
            out.append(src[i:m.end()])
            i = m.end()
            continue
        m = ORD_STRING_RE.match(src, i)
        if m:
            out.append(src[i:m.end()])
            i = m.end()
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("".join(c if c == "\n" else " " for c in src[i:j]))
            i = j
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def decode_ordinary(lit: str) -> str:
    """Decode a plain double-quoted C++ string literal's escapes (best effort)."""
    body = lit[1:-1]
    return (
        body.replace(r"\n", "\n")
        .replace(r"\t", "\t")
        .replace(r"\"", '"')
        .replace(r"\\", "\\")
    )


def tokenize(src: str) -> list[Tok]:
    toks: list[Tok] = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c.isspace():
            i += 1
            continue
        m = RAW_STRING_RE.match(src, i)
        if m:
            toks.append(Tok("STR", m.group(2), i))
            i = m.end()
            continue
        m = ORD_STRING_RE.match(src, i)
        if m:
            toks.append(Tok("STR", decode_ordinary(m.group(0)), i))
            i = m.end()
            continue
        if c == "{":
            toks.append(Tok("LBRACE", c, i))
            i += 1
            continue
        if c == "}":
            toks.append(Tok("RBRACE", c, i))
            i += 1
            continue
        if c == ",":
            toks.append(Tok("COMMA", c, i))
            i += 1
            continue
        m = IDENT_RE.match(src, i)
        if m:
            toks.append(Tok("IDENT", m.group(0), i))
            i = m.end()
            continue
        toks.append(Tok("OTHER", c, i))
        i += 1
    return toks


def matching_rbrace(toks: list[Tok], lbrace_idx: int) -> int:
    """Return the index of the RBRACE matching toks[lbrace_idx] (an LBRACE)."""
    depth = 0
    for j in range(lbrace_idx, len(toks)):
        if toks[j].kind == "LBRACE":
            depth += 1
        elif toks[j].kind == "RBRACE":
            depth -= 1
            if depth == 0:
                return j
    raise ValueError("unbalanced braces")


def split_top_level(toks: list[Tok], lo: int, hi: int) -> list[tuple[int, int]]:
    """Split toks[lo:hi] (a brace body, EXCLUSIVE of the outer braces) into
    top-level comma-separated element spans [start, end).

    Also tracks `<`/`>` depth (in addition to `{`/`}`), because
    `run_pinned`'s second argument is routinely a template type like
    `std::array<std::string, 3>{...}` — the comma inside the angle
    brackets is NOT an argument separator, but a comma-splitter that only
    understands braces would treat it as one and truncate the argument
    before it ever reaches the `{...}` literal. This is a narrow,
    call-argument-shaped heuristic (real ambiguity with `<`/`>` as
    comparison operators exists in C++ generally), acceptable here
    because every caller of this function splits either a flat literal
    list or a `fn(...)` argument list, never a boolean expression.
    """
    elems: list[tuple[int, int]] = []
    depth = 0
    angle = 0
    start = lo
    i = lo
    while i < hi:
        if toks[i].kind == "LBRACE":
            depth += 1
        elif toks[i].kind == "RBRACE":
            depth -= 1
        elif toks[i].kind == "OTHER" and toks[i].value == "<":
            angle += 1
        elif toks[i].kind == "OTHER" and toks[i].value == ">" and angle > 0:
            angle -= 1
        elif toks[i].kind == "COMMA" and depth == 0 and angle == 0:
            elems.append((start, i))
            start = i + 1
        i += 1
    if start < hi:
        elems.append((start, hi))
    return elems


def is_all_strings(toks: list[Tok], lo: int, hi: int) -> list[str]:
    """If toks[lo:hi] is a comma list where every top-level element is
    EXACTLY one STR token, return the decoded strings; else []."""
    elems = split_top_level(toks, lo, hi)
    out = []
    for a, b in elems:
        span = [t for t in toks[a:b]]
        if len(span) != 1 or span[0].kind != "STR":
            return []
        out.append(span[0].value)
    return out


VERB_TOKEN_RE = re.compile(r"^[a-z][a-z0-9_-]*$")


def literal_leaf_pair(strings: list[str]) -> tuple[str, str] | None:
    """First two elements of a pure-string group, if they look like a
    (verb, subcommand) pair. Grounding against the real CLI universe
    happens by the caller; this just filters obviously-not-a-verb shapes
    (flags, numeric ids, prose)."""
    if len(strings) < 1:
        return None
    verb = strings[0]
    if not VERB_TOKEN_RE.match(verb) or verb.startswith("-"):
        return None
    sub = "."
    if len(strings) >= 2:
        cand = strings[1]
        if VERB_TOKEN_RE.match(cand) and not cand.startswith("-"):
            sub = cand
    return (verb, sub)


def find_enclosing_ranges(toks: list[Tok], pos_of_call: int):
    """Yield (var_name, [literals], body_lo, body_hi) for every
    `for (auto const& VAR : {"a","b",...}) { ... }` range-for loop whose
    BODY textually contains the call at pos_of_call (token index)."""
    i = 0
    n = len(toks)
    while i < n:
        if (
            toks[i].kind == "IDENT" and toks[i].value == "for"
            and i + 1 < n
        ):
            # scan ahead for `( auto const& VAR : { ... } )`
            j = i + 1
            if j < n and toks[j].kind == "OTHER" and toks[j].value == "(":
                k = j + 1
                # tolerate `auto`, `const`, `&` tokens before VAR
                var = None
                while k < n and toks[k].kind != "OTHER" or (toks[k].kind == "OTHER" and toks[k].value == "&"):
                    if toks[k].kind == "IDENT" and toks[k].value not in ("auto", "const"):
                        var = toks[k].value
                    if toks[k].kind == "OTHER" and toks[k].value == ":":
                        break
                    k += 1
                    if k - j > 20:
                        break
                # find the ':' then the literal brace list then ')'
                m = k
                while m < n and not (toks[m].kind == "OTHER" and toks[m].value == ":"):
                    m += 1
                    if m - k > 5:
                        break
                if m < n and toks[m].kind == "OTHER" and toks[m].value == ":":
                    p = m + 1
                    if p < n and toks[p].kind == "LBRACE":
                        close = matching_rbrace(toks, p)
                        lits = is_all_strings(toks, p + 1, close)
                        if var and lits:
                            # find the loop body `{ ... }` after the closing `)`
                            q = close + 1
                            while q < n and not (toks[q].kind == "OTHER" and toks[q].value == ")"):
                                q += 1
                            r = q + 1
                            while r < n and toks[r].kind != "LBRACE":
                                # allow a non-brace single-statement body too;
                                # bail after a short lookahead
                                r += 1
                                if r - q > 5:
                                    break
                            if r < n and toks[r].kind == "LBRACE":
                                body_close = matching_rbrace(toks, r)
                                if r <= pos_of_call <= body_close:
                                    yield (var, lits, r, body_close)
        i += 1


def find_container_rows(toks: list[Tok], container_name: str, before_pos: int) -> list[list[str]]:
    """Locate `container_name{ ROW, ROW, ... }` (any prefix like
    `std::vector<row> const container_name`) and return each row's
    resolved args list (first all-string nested brace group per row).

    Each Catch2 `TEST_CASE` in these files commonly redeclares its own
    local `leaves`/`steps` table under the SAME name, so scanning for the
    first file-wide match would silently bind a later TEST_CASE's loop to
    an earlier, unrelated table. `before_pos` (the `for`-loop header's
    token index) anchors the search to the NEAREST preceding definition
    instead — the local-variable-shadowing rule the C++ scope itself
    enforces.
    """
    rows_out: list[list[str]] = []
    best_idx = None
    for idx, t in enumerate(toks):
        if idx >= before_pos:
            break
        if t.kind == "IDENT" and t.value == container_name:
            j = idx + 1
            if j < len(toks) and toks[j].kind == "LBRACE":
                best_idx = idx
    if best_idx is not None:
        idx = best_idx
        j = idx + 1
        if j < len(toks) and toks[j].kind == "LBRACE":
            close = matching_rbrace(toks, j)
            for a, b in split_top_level(toks, j + 1, close):
                # within this row span, find the first nested brace
                # group whose contents are all string literals. Each
                # row span itself is a `{...}` aggregate-init group
                # (its own outer braces are included in [a, b)), so
                # strip those before scanning for the NESTED args
                # sublist — otherwise the outer brace is tried first,
                # fails is_all_strings (it also contains the row's
                # tag/code/out/err fields), and the whole row gets
                # skipped without ever descending into it.
                row_toks = toks[a:b]
                if len(row_toks) >= 2 and row_toks[0].kind == "LBRACE" and row_toks[-1].kind == "RBRACE":
                    row_toks = row_toks[1:-1]
                k = 0
                while k < len(row_toks):
                    if row_toks[k].kind == "LBRACE":
                        rc = matching_rbrace(row_toks, k)
                        strs = is_all_strings(row_toks, k + 1, rc)
                        if strs:
                            rows_out.append(strs)
                            break
                        k = rc + 1
                        continue
                    k += 1
            return rows_out
    return rows_out


def extract_call_arglists(toks: list[Tok], fn_name: str, arg_index: int):
    """Yield (call_pos, arg_toks_lo, arg_toks_hi, arg_first_tok) for the
    arg at position `arg_index` (0-based) of every `fn_name(...)` call."""
    n = len(toks)
    for idx, t in enumerate(toks):
        if t.kind == "IDENT" and t.value == fn_name:
            j = idx + 1
            if not (j < n and toks[j].kind == "OTHER" and toks[j].value == "("):
                continue
            # find matching close paren, tracking nested parens/braces
            depth = 1
            k = j + 1
            paren_depth = {j: 1}
            pd = 1
            while k < n and pd > 0:
                if toks[k].kind == "OTHER" and toks[k].value == "(":
                    pd += 1
                elif toks[k].kind == "OTHER" and toks[k].value == ")":
                    pd -= 1
                elif toks[k].kind == "LBRACE":
                    # skip whole brace group so its internal commas don't
                    # get mistaken for arg separators
                    close = matching_rbrace(toks, k)
                    k = close
                k += 1
            close_paren = k - 1
            # split top-level (paren_depth==0 relative) commas between j+1..close_paren
            args = split_top_level(toks, j + 1, close_paren)
            if arg_index < len(args):
                a, b = args[arg_index]
                yield (idx, a, b)


def resolve_arglist(toks: list[Tok], lo: int, hi: int) -> list[str] | None:
    """Resolve an arg-list span to its LEADING (verb, subcommand)
    positional string literals, if it's a brace-init list (bare `{...}`,
    `std::array<...>{...}`, or `std::vector<...>{...}`) whose first
    element is a string literal. Returns None if the span isn't a
    brace-init list at all, or its first element isn't a literal (e.g. a
    bare identifier / field access) — the caller handles those cases
    separately.

    Deliberately positional, NOT "every element must be a literal": a
    real dispatch call frequently mixes a literal verb/subcommand with a
    runtime-computed positional argument afterward — e.g. `dispatch(fx,
    {"synthesize", (fx.root / "repo").string(), "--json"})`. Requiring
    every element to be a plain string literal would silently drop that
    leaf's verb-only credit even though argv[0] is pinned. Only the first
    two POSITIONS are examined (argv[0]=verb, argv[1]=subcommand), never
    an arbitrary later literal in the list — taking any later string
    that happens to look like a subcommand would misattribute leaves
    (trap 2's adjacency mistake in a different shape).
    """
    span = toks[lo:hi]
    for i, t in enumerate(span):
        if t.kind == "LBRACE":
            close_rel = matching_rbrace(span, i)
            elems = split_top_level(span, i + 1, close_rel)
            if not elems:
                return None
            first = span[elems[0][0]:elems[0][1]]
            if len(first) != 1 or first[0].kind != "STR":
                return None
            result = [first[0].value]
            if len(elems) >= 2:
                second = span[elems[1][0]:elems[1][1]]
                if len(second) == 1 and second[0].kind == "STR":
                    result.append(second[0].value)
            return result
    return None


def is_field_ref(toks: list[Tok], lo: int, hi: int, field_names=("args", "argv")):
    """If span is exactly `IDENT . field`, return (IDENT, field)."""
    span = [t for t in toks[lo:hi]]
    if len(span) == 3 and span[0].kind == "IDENT" and span[1].kind == "OTHER" and span[1].value == "." and span[2].kind == "IDENT":
        if span[2].value in field_names:
            return (span[0].value, span[2].value)
    return None


def find_loop_var_container(toks: list[Tok], var_name: str, call_pos: int):
    """Find `for (auto const& VAR : CONTAINER)` whose body contains call_pos.

    Returns (CONTAINER identifier name, token index of the `for` keyword)
    so the caller can anchor the container's OWN definition search to the
    nearest preceding declaration (see `find_container_rows`) rather than
    the first file-wide match — every `TEST_CASE` here redeclares its own
    local `steps`/`leaves` table under the same name.
    """
    n = len(toks)
    i = 0
    while i < n:
        if toks[i].kind == "IDENT" and toks[i].value == "for":
            j = i + 1
            if j < n and toks[j].kind == "OTHER" and toks[j].value == "(":
                k = j + 1
                found_var = None
                while k < n and not (toks[k].kind == "OTHER" and toks[k].value == ":"):
                    if toks[k].kind == "IDENT" and toks[k].value not in ("auto", "const"):
                        found_var = toks[k].value
                    k += 1
                    if k - j > 15:
                        break
                if k < n and toks[k].kind == "OTHER" and toks[k].value == ":" and found_var == var_name:
                    p = k + 1
                    if p < n and toks[p].kind == "IDENT":
                        container = toks[p].value
                        q = p + 1
                        while q < n and not (toks[q].kind == "OTHER" and toks[q].value == ")"):
                            q += 1
                        r = q + 1
                        while r < n and toks[r].kind != "LBRACE":
                            r += 1
                            if r - q > 5:
                                break
                        if r < n and toks[r].kind == "LBRACE":
                            body_close = matching_rbrace(toks, r)
                            if r <= call_pos <= body_close:
                                return (container, i)
        i += 1
    return None


def extract_from_file(path: str) -> set[tuple[str, str]]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    clean = strip_comments(raw)
    toks = tokenize(clean)

    out: set[tuple[str, str]] = set()
    out |= _extract_dispatch(toks)
    out |= _extract_run_pinned(toks)
    return out


def _extract_dispatch(toks: list[Tok]) -> set[tuple[str, str]]:
    # `dispatch`'s own arg position for the argv list is NOT uniform
    # across files: most `*_leaves.t.cpp` files define
    # `dispatch(const fixture& fx, std::vector<std::string> args)` (argv
    # at position 1), but `dispatch.t.cpp` defines its own
    # `dispatch(std::vector<std::string> args, ...)` with no fixture
    # parameter (argv at position 0) — `dispatch({"explore"})` is the
    # call shape there. Trying both positions per file is simpler and
    # more robust than hard-coding a position per filename: whichever
    # position is a brace-init list resolves; the other (an identifier
    # like `fx`, or nothing — a call with fewer args) yields nothing.
    out: set[tuple[str, str]] = set()
    for arg_index in (0, 1):
        out |= _extract_dispatch_at(toks, arg_index)
    return out


def _extract_dispatch_at(toks: list[Tok], arg_index: int) -> set[tuple[str, str]]:
    out: set[tuple[str, str]] = set()
    for call_idx, lo, hi in extract_call_arglists(toks, "dispatch", arg_index):
        strs = resolve_arglist(toks, lo, hi)
        if strs:
            pair = literal_leaf_pair(strs)
            if pair:
                out.add(pair)
        # ALWAYS also check for the single-var loop substitution shape —
        # `{"verb", VAR}` / `{"verb", std::string{VAR}}` — even when
        # resolve_arglist already yielded a verb-only credit above: the
        # substitution below is what recovers the SUBCOMMAND (e.g.
        # `scope pop` / `scope clear` from `scope_leaves.t.cpp:392`'s
        # `for (auto const& verb : {"use","pop","clear"}) dispatch(fx,
        # {"scope", std::string{verb}})`), which the verb-only credit
        # above does not carry.
        span = toks[lo:hi]
        # find outer LBRACE
        for i, t in enumerate(span):
            if t.kind == "LBRACE":
                close_rel = matching_rbrace(span, i)
                elems = split_top_level(span, i + 1, close_rel)
                if len(elems) >= 1 and span[elems[0][0]].kind == "STR":
                    verb = span[elems[0][0]].value
                    if len(elems) >= 2:
                        sub_toks = span[elems[1][0]:elems[1][1]]
                        var = None
                        if len(sub_toks) == 1 and sub_toks[0].kind == "IDENT":
                            var = sub_toks[0].value
                        elif any(t2.kind == "IDENT" for t2 in sub_toks):
                            idents = [t2.value for t2 in sub_toks if t2.kind == "IDENT"]
                            # std::string{VAR} -> idents = ["std","string","VAR"]
                            if idents:
                                var = idents[-1]
                        if var:
                            for found_var, lits, blo, bhi in find_enclosing_ranges(toks, call_idx):
                                if found_var == var:
                                    for lit in lits:
                                        pair = literal_leaf_pair([verb, lit])
                                        if pair:
                                            out.add(pair)
                break
    return out


def _extract_run_pinned(toks: list[Tok]) -> set[tuple[str, str]]:
    out: set[tuple[str, str]] = set()
    bin_spans = list(extract_call_arglists(toks, "run_pinned", 0))
    arg_spans = list(extract_call_arglists(toks, "run_pinned", 1))
    bin_by_call = {idx: (lo, hi) for idx, lo, hi in bin_spans}
    for call_idx, lo, hi in arg_spans:
        bin_lo, bin_hi = bin_by_call.get(call_idx, (None, None))
        if bin_lo is None:
            continue
        bin_span = toks[bin_lo:bin_hi]
        if not (bin_span and bin_span[0].kind == "IDENT" and bin_span[0].value == "cpp_bin"):
            continue  # only the `planar` binary is in this gate's universe
        strs = resolve_arglist(toks, lo, hi)
        if strs:
            pair = literal_leaf_pair(strs)
            if pair:
                out.add(pair)
            continue
        field = is_field_ref(toks, lo, hi)
        if field:
            var_name, _field = field
            found = find_loop_var_container(toks, var_name, call_idx)
            if found:
                container, for_idx = found
                for row_strs in find_container_rows(toks, container, for_idx):
                    pair = literal_leaf_pair(row_strs)
                    if pair:
                        out.add(pair)
    return out


def main() -> int:
    files = sys.argv[1:]
    all_pairs: set[tuple[str, str]] = set()
    for path in files:
        try:
            all_pairs |= extract_from_file(path)
        except Exception as exc:  # pragma: no cover - defensive
            print(f"coverage-extract: warning: failed to parse {path}: {exc}", file=sys.stderr)
    for verb, sub in sorted(all_pairs):
        print(f"{verb} {sub}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
