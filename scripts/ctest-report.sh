#!/usr/bin/env bash
#
# ctest-report.sh — run the C++ suite and REPORT ITS SKIP TALLY (plan 996,
# task 6071).
#
# ## Why this exists
#
# `catch_discover_tests` sets SKIP_RETURN_CODE 4 (cmake/module.cmake), so a
# skipped case is not a failure and `ctest` still exits 0. That is the right
# default — a developer who has not built `zig/` should not be blocked — but
# it means a run where the entire differential lane was skipped reports the
# same way as one where all of it passed. "1743 tests passed" and "1717
# passed, 26 skipped" are very different claims about what was verified, and
# only the second one is visible if you go looking for it.
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
# To make a missing oracle a hard FAILURE rather than a skip, set
# PLANAR_PARITY_STRICT=1 (see src/cmd/parity_strict.hpp). The two mechanisms
# are complementary: the env var refuses at the case, this script reports at
# the run.
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
  echo "A skipped case asserted NOTHING. If these are oracle-gated, build the"
  echo "reference with 'make build' and re-run, or set PLANAR_PARITY_STRICT=1"
  echo "to make an absent oracle a failure instead."
fi

echo "ctest exit: $CTEST_RC"

if [ -n "$MAX_SKIPS" ] && [ "$SKIPPED_COUNT" -gt "$MAX_SKIPS" ]; then
  echo "FAIL: $SKIPPED_COUNT skipped exceeds --max-skips $MAX_SKIPS"
  exit 1
fi

exit "$CTEST_RC"
