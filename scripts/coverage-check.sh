#!/usr/bin/env bash
#
# coverage-check.sh — leaf (verb, subcommand) integration-test coverage gate.
#
# Walks the CLI's verb surface, extracts (verb, subcommand) tuples
# exercised by `integration_tests/*.zig`, and reports the ratio. Fails
# when the ratio drops below the recorded baseline so a new leaf added
# without a scenario test trips CI immediately.
#
# Companion to CLAUDE.md "Integration test methodology": new verbs /
# subcommands require integration-test coverage as part of the same PR.
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

# Expand test-file globs at runtime so a missing scenarios/ dir is not
# fatal (the directory is added incrementally as workflows are scripted).
collect_test_files() {
  ls "$REPO_ROOT"/integration_tests/*.zig 2>/dev/null || true
  ls "$REPO_ROOT"/integration_tests/scenarios/*.zig 2>/dev/null || true
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

PLANAR_BIN="${PLANAR_BIN:-$REPO_ROOT/bin/planar}"
if [[ ! -x "$PLANAR_BIN" ]]; then
  echo "coverage-check: $PLANAR_BIN not executable — run 'make build' first" >&2
  exit 1
fi

# ---------- enumerate leaf (verb, subcommand) pairs ----------
TMP_ALL=$(mktemp)
trap 'rm -f "$TMP_ALL" "$TMP_EXERCISED" "$TMP_UNCOVERED"' EXIT
TMP_EXERCISED=$(mktemp)
TMP_UNCOVERED=$(mktemp)

# Verbs we deliberately exclude from coverage tracking:
#   - completion: emits shell scripts; not amenable to scenario testing
#   - version: emits a single line
#   - sync: hits live external systems
#   - planar: doesn't exist as a real verb (appears in --help output as
#     a synonym for the bare binary in some renderings)
EXCLUDE_VERBS="completion version sync planar init"

is_excluded() {
  local v="$1"
  for e in $EXCLUDE_VERBS; do
    [[ "$v" == "$e" ]] && return 0
  done
  return 1
}

for v in $("$PLANAR_BIN" --help 2>&1 | awk '/^  [a-z]/ { print $1 }'); do
  if is_excluded "$v"; then continue; fi
  subs=$("$PLANAR_BIN" "$v" --help 2>&1 \
    | awk '/^COMMANDS:/{f=1;next} f && /^$/{exit} f && /^  [a-z]/{print $1}')
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

# ---------- extract exercised pairs from integration_tests/ ----------
# Walk every *_test.zig file under integration_tests/, scan each `&.{`
# slice literal across line boundaries, and capture the first two
# string-literal positional args as (verb, subcommand). A `&.{ "verb" }`
# with no second arg counts as (verb, "."). The Python pass handles
# multi-line `&.{ \n "verb", \n "sub", ... }` forms which a bare grep
# regex would miss.
TEST_FILES=$(collect_test_files)
if [[ -n "$TEST_FILES" ]]; then
  python3 - "$TMP_EXERCISED" $TEST_FILES <<'PY'
import re, sys

out_path = sys.argv[1]
files = sys.argv[2:]

# Match `&.{` followed by anything up to the closing `}`. We constrain
# the body to no nested `{` to keep the matcher simple — the harness
# convention is flat positional arg lists.
slice_re = re.compile(r"&\.\{([^{}]*)\}", re.DOTALL)
str_re   = re.compile(r'"([a-z][-a-z]*)"')

exercised = set()
for path in files:
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        continue
    for m in slice_re.finditer(text):
        body = m.group(1)
        strings = str_re.findall(body)
        if not strings:
            continue
        verb = strings[0]
        sub = strings[1] if len(strings) > 1 else "."
        exercised.add((verb, sub))

with open(out_path, "w", encoding="utf-8") as f:
    for v, s in sorted(exercised):
        f.write(f"{v} {s}\n")
PY
fi

# Fold in any top-level verb appearance into the "<verb> ." sentinel
# so leaf verbs (no subcommand) are picked up even when only their
# bare name appears (e.g. `&.{ "tree", "--all-scopes" }` where the
# second slot is a flag, not a subcommand).
for v in $("$PLANAR_BIN" --help 2>&1 | awk '/^  [a-z]/ { print $1 }'); do
  if is_excluded "$v"; then continue; fi
  if [[ -n "$TEST_FILES" ]] && grep -qE "\&\.\{\s*\"$v\"" $TEST_FILES 2>/dev/null; then
    echo "$v ." >> "$TMP_EXERCISED"
  fi
done
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
      echo "  See CLAUDE.md 'Integration test methodology' § contribution policy." >&2
      echo "" >&2
      echo "  Currently uncovered leaves include:" >&2
      head -20 "$TMP_UNCOVERED" | sed 's/^/    /' >&2
      exit 1
    fi
    echo "OK"
    exit 0
    ;;
esac
