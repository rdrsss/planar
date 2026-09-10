#!/usr/bin/env bash
#
# ctest-report.sh — run the C++ suite and REPORT ITS SKIP TALLY (plan 996,
# task 6071).
#
# ## Why this exists
#
# `catch_discover_tests` sets SKIP_RETURN_CODE 4 (cmake/module.cmake), so a
# skipped case is not a failure and `ctest` still exits 0. That was the right
# default while a case could legitimately be unrunnable on a given checkout,
# but it means a run where an entire lane was skipped reports the same way as
# one where all of it passed. "1743 tests passed" and "1717 passed, 26
# skipped" are very different claims about what was verified, and only the
# second one is visible if you go looking for it.
#
# AS OF THE M10 CUTOVER (task 6045, decisions 963/982) THE EXPECTED TALLY IS
# ZERO. Every skip this repo ever reported was oracle-conditional, and there
# is no oracle any more: the last such case, `planar-watch parity: the six
# read verbs agree with the oracle over a seeded database`, was deleted by
# decision 1034 in the same commit. A nonzero tally now means a NEW skip was
# introduced, which is exactly what this script is for. Run it with
# `--max-skips 0` to make that a failure.
#
# This wrapper always PRINTS the tally, names the skipped cases, and (with
# --max-skips) can fail when there are more than expected. It does not change
# ctest's own pass/fail verdict.
#
# ## Usage
#
#   scripts/ctest-report.sh [--build-dir DIR] [--max-skips N] [-- <ctest args>]
#
#   --build-dir DIR   default: build/debug
#   --max-skips N     exit non-zero if more than N cases skipped
#
# `PLANAR_PARITY_STRICT` and `src/cmd/parity_strict.hpp` are GONE (task
# 6045). They existed to turn an absent oracle from a skip into a failure at
# the case; with the oracle deleted there is nothing for them to refuse, and
# `--max-skips 0` covers the remaining need at the run.
set -uo pipefail

BUILD_DIR=build/debug
MAX_SKIPS=""
CTEST_ARGS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --max-skips) MAX_SKIPS="$2"; shift 2 ;;
    --) shift; CTEST_ARGS=("$@"); break ;;
    *) CTEST_ARGS+=("$1"); shift ;;
  esac
done

LOG=$(mktemp -t planar-ctest-report)
trap 'rm -f "$LOG"' EXIT

ctest --test-dir "$BUILD_DIR" --output-on-failure "${CTEST_ARGS[@]+"${CTEST_ARGS[@]}"}" 2>&1 | tee "$LOG"
CTEST_RC=${PIPESTATUS[0]}

# Catch2's skip surfaces through ctest as the "Skipped" disposition. Count
# the per-test result lines rather than parsing the summary, so the tally is
# derived from the same lines that name the cases below.
SKIPPED_LINES=$(grep -E '\*\*\*Skipped' "$LOG" || true)
SKIPPED_COUNT=$(printf '%s' "$SKIPPED_LINES" | grep -c . || true)

echo
echo "=== skip tally ==="
echo "skipped: $SKIPPED_COUNT"
if [ "$SKIPPED_COUNT" -gt 0 ]; then
  printf '%s\n' "$SKIPPED_LINES" | sed 's/^/  /'
  echo
  echo "A skipped case asserted NOTHING. Since the M10 cutover (task 6045) the"
  echo "expected tally is ZERO — every historical skip was oracle-conditional"
  echo "and the oracle is deleted. Investigate each case above."
fi

echo "ctest exit: $CTEST_RC"

if [ -n "$MAX_SKIPS" ] && [ "$SKIPPED_COUNT" -gt "$MAX_SKIPS" ]; then
  echo "FAIL: $SKIPPED_COUNT skipped exceeds --max-skips $MAX_SKIPS"
  exit 1
fi

exit "$CTEST_RC"
