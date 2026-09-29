#!/usr/bin/env bash
#
# coverage-check.sh — leaf (verb, subcommand) integration-test coverage gate.
#
# Walks the `planar` CLI's verb surface, extracts (verb, subcommand) tuples
# exercised by the C++ Catch2 test corpus under `src/`, and reports the
# ratio. Fails when the ratio drops below the recorded baseline so a new
# leaf added without a test trips CI immediately.
#
# Companion to docs/testing.md § Contribution policy: new verbs /
# subcommands require integration-test coverage as part of the same PR.
#
# ## Re-pointed from the Zig corpus to the C++ corpus (task 6436, decision
# ## 1035)
#
# This gate originally measured `zig/integration_tests/*.zig` (the
# `&.{ "verb", "sub" }` slice-literal scan). Task 6548 deleted the
# redundant Zig integration blocks the C++ port had superseded, which
# correctly made this gate start failing — it was reporting a shrinking
# corpus accurately, not malfunctioning. Task 6045 deletes `zig/` entirely
# next, at which point there would be no corpus left to measure at all.
#
# The decision (see task 6436's body and decision 1035): RE-POINT at the
# C++ corpus rather than retire the gate. The rule it enforces —
# docs/testing.md's "a change adding a verb/subcommand must add coverage in the same
# PR" — is still worth a mechanical guard, and this gate is the only one
# that exists. Retiring it would mean nothing catches the next silent
# regression the way task 6546 caught eight of them.
#
# The extraction pass changed shape to match: it now scans
# `dispatch(fx, {"verb", "sub", ...})` calls (the in-process
# `*_leaves.t.cpp` / `*_leaf.t.cpp` Catch2 tests) and
# `run_pinned(cpp_bin(), <arg-list>, ...)` calls (the cross-process
# black-box tests) instead of Zig slice literals. See
# `scripts/coverage-extract.py` for the extractor itself and its header
# comment for the two traps this plan already hit once each:
#
#   1. Table-driven loops (a range-for over a literal set, or a
#      `std::vector<row> const rows{ {tag, {args...}, ...}, ... }` table)
#      genuinely exercise a leaf through a variable, not a literal at the
#      call site. The extractor resolves both of the shapes this corpus
#      actually uses; anything else is a documented remaining blind spot
#      that undercounts (never overcounts).
#   2. A leaf NAME appearing as a JSON substring inside the pinned
#      `schema` catalog raw-string literal (`parity.t.cpp`'s
#      `R"CATALOG...(...)"`) is NOT coverage. The extractor tokenizes C++
#      raw strings as one opaque token specifically so that blob is never
#      split into fake adjacent "string literals".
#
# Usage
#   scripts/coverage-check.sh                # check against baseline
#   scripts/coverage-check.sh --update       # rewrite baseline to current
#   scripts/coverage-check.sh --report       # human-readable per-verb table
#   scripts/coverage-check.sh --json         # machine-readable summary
#   scripts/coverage-check.sh --uncovered    # print only uncovered leaves

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="$REPO_ROOT/scripts/coverage-baseline.txt"
EXTRACTOR="$REPO_ROOT/scripts/coverage-extract.py"

# Scan every Catch2 test file under src/ that plausibly dispatches CLI
# argv — i.e. calls a local `dispatch(...)` helper or the cross-process
# `run_pinned(...)` helper. Restricting to this grep pre-filter (rather
# than every `*.t.cpp` file) keeps the extractor's tokenizer off files
# that can never contribute a leaf, without hand-maintaining a file list
# that will drift as new `*_leaves.t.cpp` files are added.
collect_test_files() {
  find "$REPO_ROOT/src" -iname '*.t.cpp' -print0 \
    | xargs -0 grep -l 'dispatch(\|run_pinned(' 2>/dev/null | sort
}

MODE="check"
for arg in "$@"; do
  case "$arg" in
    --update) MODE="update" ;;
    --report) MODE="report" ;;
    --json) MODE="json" ;;
    --uncovered) MODE="uncovered" ;;
    --help|-h) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "coverage-check: unknown arg '$arg' (try --help)" >&2; exit 64 ;;
  esac
done

# Prefer `make coverage`, which depends on `build` and therefore measures a
# binary that matches the working tree. Invoked directly, this script measures
# whatever ./bin/planar currently holds — and a STALE binary reports a clean
# ratio against a CLI surface that no longer exists, because verbs added since
# that build are invisible to the enumeration below. The check below only
# catches a MISSING binary, not an out-of-date one.
PLANAR_BIN="${PLANAR_BIN:-$REPO_ROOT/bin/planar}"

# Point every probe at a scratch database.
#
# A planar binary opens and MIGRATES its database before it does anything —
# even `--help` does, and this script runs `--help` once per verb. Without an
# override those probes resolve to the operator's real ~/.planar/planar.db and
# silently migrate it to this checkout's schema, which breaks every installed
# binary on the machine until someone rolls the migration back by hand.
#
# The agent database (decision 1181; ~/.planar/agent.db, override
# PLANAR_AGENT_DB) migrates on first open the same way, so it is pinned into
# the same scratch directory rather than left to the HOME fallback (task
# 6996). The EXIT trap below removes the directory and both files with it.
COVERAGE_DB_DIR="$(mktemp -d)"
export PLANAR_DB="$COVERAGE_DB_DIR/coverage-probe.db"
export PLANAR_AGENT_DB="$COVERAGE_DB_DIR/agent.db"
if [[ ! -x "$PLANAR_BIN" ]]; then
  echo "coverage-check: $PLANAR_BIN not executable — run 'make build' first" >&2
  exit 1
fi

# ---------- enumerate leaf (verb, subcommand) pairs ----------
TMP_ALL=$(mktemp)
# One EXIT trap only: a second `trap ... EXIT` REPLACES this one rather than
# adding to it, which would leak whichever directory lost the race.
trap 'rm -f "$TMP_ALL" "$TMP_EXERCISED" "$TMP_UNCOVERED"; rm -rf "$COVERAGE_DB_DIR"' EXIT
TMP_EXERCISED=$(mktemp)
TMP_UNCOVERED=$(mktemp)

# Verbs we deliberately exclude from coverage tracking:
#   - completion: emits shell scripts; not amenable to scenario testing
#   - version: emits a single line
#
# NOTE: 'sync' and 'planar' were pruned here (task 6437). 'sync' moved off
# the `planar` binary onto `planar-ext` this session (task 6419) and no
# longer appears as a top-level verb in `planar --help`; 'planar' never
# matched a real top-level row. Both exclusions were dead — pruning them is
# behavior-preserving (verified: `planar --help` SUBCOMMANDS has no 'sync'
# or 'planar' row, so is_excluded() never matched either token before this
# change either).
EXCLUDE_VERBS="completion version init"

is_excluded() {
  local v="$1"
  for e in $EXCLUDE_VERBS; do
    [[ "$v" == "$e" ]] && return 0
  done
  return 1
}

for v in $("$PLANAR_BIN" --help 2>&1 | awk '/^  [a-z]/ { print $1 }'); do
  if is_excluded "$v"; then continue; fi
  # CLI11 (the current parser, since decision 948) emits "SUBCOMMANDS:" as
  # the section heading, not "COMMANDS:" — the latter was etcli-zig's
  # heading and never matched CLI11's --help output. With the wrong
  # heading this awk block set `f` for zero help pages, so every verb's
  # subcommand list read back empty and the gate silently measured only
  # top-level verbs (see scripts/coverage-baseline.txt history / task 6433).
  subs=$("$PLANAR_BIN" "$v" --help 2>&1 \
    | awk '/^SUBCOMMANDS:/{f=1;next} f && /^$/{exit} f && /^  [a-z]/{print $1}')
  if [[ -z "$subs" ]]; then
    # Leaf top-level verb (no subcommands). Track with sentinel "." sub.
    echo "$v ." >> "$TMP_ALL"
  else
    while IFS= read -r s; do
      [[ -z "$s" ]] && continue
      echo "$v $s" >> "$TMP_ALL"
    done <<< "$subs"
  fi
done

sort -u -o "$TMP_ALL" "$TMP_ALL"

# ---------- extract exercised pairs from the C++ Catch2 corpus ----------
# scripts/coverage-extract.py does the real work: it tokenizes each file
# (raw strings as one opaque token, comments stripped) and matches CALL
# SHAPE — `dispatch(fx, {...})` / `run_pinned(cpp_bin(), <arg-list>, ...)`
# — never string adjacency, so the pinned `schema` catalog's giant JSON
# raw-string literal (which contains every verb/subcommand name as a
# substring) cannot masquerade as coverage. It also resolves the two
# table-driven-loop shapes this corpus actually uses (a range-for over a
# literal set; a `std::vector<row> const rows{ {tag, {args...}, ...} }`
# table iterated via `row.args`/`row.argv`) — see its header comment for
# the full contract and the leftover, deliberately under-count-only blind
# spots.
TEST_FILES=$(collect_test_files)
if [[ -n "$TEST_FILES" ]]; then
  # shellcheck disable=SC2086 -- word-splitting the file list is intended
  python3 "$EXTRACTOR" $TEST_FILES > "$TMP_EXERCISED"
fi
sort -u -o "$TMP_EXERCISED" "$TMP_EXERCISED"

# ---------- compute uncovered ----------
comm -23 "$TMP_ALL" "$TMP_EXERCISED" > "$TMP_UNCOVERED"

TOTAL=$(wc -l < "$TMP_ALL" | tr -d ' ')
EXERCISED=$(comm -12 "$TMP_ALL" "$TMP_EXERCISED" | wc -l | tr -d ' ')
UNCOVERED=$(wc -l < "$TMP_UNCOVERED" | tr -d ' ')

# ---------- mode dispatch ----------
case "$MODE" in
  json)
    printf '{"total":%d,"exercised":%d,"uncovered":%d}\n' "$TOTAL" "$EXERCISED" "$UNCOVERED"
    exit 0
    ;;
  report)
    echo "total leaves:    $TOTAL"
    echo "exercised:       $EXERCISED ($((EXERCISED * 100 / TOTAL))%)"
    echo "uncovered:       $UNCOVERED"
    echo
    echo "Per-verb uncovered counts (excluded: $EXCLUDE_VERBS):"
    join -1 1 -2 1 -a 1 -e 0 -o '0,1.2,2.2' \
      <(cut -d' ' -f1 "$TMP_ALL" | sort | uniq -c | awk '{print $2" "$1}') \
      <(cut -d' ' -f1 "$TMP_UNCOVERED" | sort | uniq -c | awk '{print $2" "$1}') \
      | awk '{printf "  %-15s %d/%d uncovered\n", $1, $3, $2}'
    exit 0
    ;;
  uncovered)
    cat "$TMP_UNCOVERED"
    exit 0
    ;;
  update)
    {
      echo "# scripts/coverage-baseline.txt — integration-test leaf coverage baseline."
      echo "#"
      echo "# One line: 'exercised total'. coverage-check.sh fails when the"
      echo "# exercised count drops below this number or total grows without a"
      echo "# matching exercised increase. Update via:"
      echo "#   make build && scripts/coverage-check.sh --update"
      echo "#"
      echo "# Excluded verbs (intentional, not counted in total):"
      echo "#   $EXCLUDE_VERBS"
      echo ""
      echo "$EXERCISED $TOTAL"
    } > "$BASELINE"
    echo "coverage baseline updated: $EXERCISED/$TOTAL exercised"
    exit 0
    ;;
  check)
    if [[ ! -s "$BASELINE" ]]; then
      echo "coverage-check: $BASELINE not found — run 'scripts/coverage-check.sh --update' to seed it" >&2
      exit 1
    fi
    read -r base_exercised base_total < <(grep -v '^#' "$BASELINE" | grep -v '^$' | head -1)
    echo "coverage-check: exercised=$EXERCISED/$TOTAL  baseline=$base_exercised/$base_total"

    if [[ "$EXERCISED" -lt "$base_exercised" ]]; then
      echo "FAIL: exercised count dropped from $base_exercised to $EXERCISED" >&2
      echo "  Newly-uncovered leaves likely; check 'scripts/coverage-check.sh --uncovered'" >&2
      exit 1
    fi
    if [[ "$TOTAL" -gt "$base_total" && "$EXERCISED" -le "$base_exercised" ]]; then
      ADDED=$((TOTAL - base_total))
      echo "FAIL: $ADDED new leaf(s) added since baseline but exercised count did not grow." >&2
      echo "  New CLI surfaces require integration-test coverage in the same PR." >&2
      echo "  See docs/testing.md § Contribution policy." >&2
      echo "" >&2
      echo "  Currently uncovered leaves include:" >&2
      head -20 "$TMP_UNCOVERED" | sed 's/^/    /' >&2
      exit 1
    fi
    echo "OK"
    exit 0
    ;;
esac
