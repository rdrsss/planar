#!/usr/bin/env bash
#
# bench-matrix-agent-flags-test.sh — tests for the coder/reviewer spawn-flag
# split and the coder_brief "real implementation task" framing.
#
# Test plan:
#   T1: Coder spawn path uses --dangerously-skip-permissions (grep
#       spawn_agent / spawn_agent_watchdog invocation).
#   T2: Reviewer spawn path does NOT contain --dangerously-skip-permissions
#       and does NOT contain --permission-mode (no bypass flag).
#   T3: coder_brief output does NOT contain the abused "no-change task is a
#       valid outcome" phrasing.
#   T4: coder_brief output DOES contain "real implementation task" or
#       equivalent "REAL IMPLEMENTATION TASK" framing.
#   T5: coder_brief output DOES contain "UNIMPLEMENTED" (the replacement for
#       the no-op escape hatch).
#   T6: Regression — all existing suites still pass (harvest, resume, m3,
#       m4, b1, base).
#
# NO real claude invocations. Assertions are grep/source-level.
# Exits non-zero on any assertion failure.
#
# Usage: scripts/bench-matrix-agent-flags-test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
# Use the repo root as the corpus repo (a real git repo) so the top-level
# PLANNING_CWD derivation inside bench-matrix.sh (git worktree list) succeeds
# when the matrix is sourced in wrapper scripts.
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

pass=0; fail=0
ok()  { printf 'PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad() { printf 'FAIL: %s\n' "$*"; fail=$((fail + 1)); }

# trimmed_matrix <dest> — strip the trailing `main "$@"` so sourcing is safe.
trimmed_matrix() {
  local dest="$1"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$dest"
  printf '%s' "$dest"
}

# run_coder_brief <home> <trimmed> — invoke coder_brief via a wrapper script
# that sets the required env vars before sourcing the matrix, then prints the
# brief to stdout. Using a wrapper avoids the set -euo pipefail + pipefail
# interaction when sourcing the matrix directly inside $() command
# substitution (git -C <non-git-dir> | awk pipeline exits non-zero under
# pipefail; the wrapper sets BENCH_CORPUS_REPO to a real repo to avoid this).
run_coder_brief() {
  local home="$1" trimmed="$2"
  local wrapper="$home/brief-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$REPO_ROOT"
export PLANAR_DB_OVERRIDE="$home/exp.db"
export PLANAR_CONFIG_PATH_OVERRIDE="$home/config.toml"
# shellcheck disable=SC1090
source "$trimmed"
coder_brief "9999" "stub problem statement"
WRAP
  chmod +x "$wrapper"
  bash "$wrapper"
}

# ===========================================================================
# TEST 1 — Coder spawn uses --dangerously-skip-permissions
# ===========================================================================
test_coder_has_bypass_flag() {
  printf '\n=== AGENT-FLAGS TEST 1: coder spawn uses --dangerously-skip-permissions ===\n'

  # spawn_agent is the coder path. The call to spawn_agent_watchdog inside it
  # must pass "--dangerously-skip-permissions" as extra_flags.
  grep -qF -- '--dangerously-skip-permissions' "$MATRIX" \
    && ok "T1a: --dangerously-skip-permissions found in bench-matrix.sh" \
    || bad "T1a: --dangerously-skip-permissions NOT found in bench-matrix.sh"

  # The coder's spawn_agent_watchdog call must carry the bypass flag.
  grep -A2 'Coder gets --dangerously-skip-permissions' "$MATRIX" \
    | grep -qF 'spawn_agent_watchdog' \
    && ok "T1b: coder spawn_agent_watchdog call follows the bypass-flag comment" \
    || bad "T1b: could not confirm coder spawn_agent_watchdog follows bypass-flag comment"

  # Confirm the bypass flag is on the coder call specifically (not reviewer).
  local coder_line; coder_line="$(grep 'spawn_agent_watchdog.*dangerously-skip-permissions' "$MATRIX" || true)"
  [ -n "$coder_line" ] \
    && ok "T1c: spawn_agent_watchdog called with --dangerously-skip-permissions on coder line" \
    || bad "T1c: spawn_agent_watchdog + --dangerously-skip-permissions NOT found on one line"
}

# ===========================================================================
# TEST 2 — Reviewer spawn has NO bypass flag
# ===========================================================================
test_reviewer_no_bypass_flag() {
  printf '\n=== AGENT-FLAGS TEST 2: reviewer spawn has no bypass flag ===\n'

  # T2a: The reviewer function spawn_agent_raw must not contain the bypass flag.
  # We grep the raw source file for the reviewer-specific watchdog call (which
  # passes "" as extra_flags) and verify it does NOT carry the bypass flag.
  # The coder call is on the line with "--dangerously-skip-permissions"; the
  # reviewer call is the one with "" as the 4th argument.
  local reviewer_watchdog_line
  reviewer_watchdog_line="$(grep 'spawn_agent_watchdog.*""' "$MATRIX" || true)"

  [ -n "$reviewer_watchdog_line" ] \
    && ok "T2a: reviewer spawn_agent_watchdog call with empty extra_flags found" \
    || bad "T2a: reviewer spawn_agent_watchdog empty-flag call NOT found in source"

  # T2b: that reviewer line must NOT contain --dangerously-skip-permissions.
  if printf '%s\n' "$reviewer_watchdog_line" | grep -qF -- '--dangerously-skip-permissions'; then
    bad "T2b: reviewer watchdog call contains --dangerously-skip-permissions (should be absent)"
  else
    ok "T2b: reviewer watchdog call does NOT contain --dangerously-skip-permissions"
  fi

  # T2c: that reviewer line must NOT contain --permission-mode (belt-and-suspenders).
  if printf '%s\n' "$reviewer_watchdog_line" | grep -qF -- '--permission-mode'; then
    bad "T2c: reviewer watchdog call contains --permission-mode (should be absent)"
  else
    ok "T2c: reviewer watchdog call does NOT contain --permission-mode"
  fi

  # T2d: there is exactly ONE watchdog call with "" (reviewer) and exactly ONE
  # with --dangerously-skip-permissions (coder). Confirm counts.
  local empty_flag_count bypass_count
  empty_flag_count="$(grep -c 'spawn_agent_watchdog.*""' "$MATRIX" || true)"
  bypass_count="$(grep -c 'spawn_agent_watchdog.*dangerously-skip-permissions' "$MATRIX" || true)"

  [ "$empty_flag_count" -ge 1 ] \
    && ok "T2d: at least one reviewer spawn_agent_watchdog with empty extra_flags (count=$empty_flag_count)" \
    || bad "T2d: no reviewer spawn_agent_watchdog with empty extra_flags found"

  [ "$bypass_count" -ge 1 ] \
    && ok "T2e: at least one coder spawn_agent_watchdog with bypass flag (count=$bypass_count)" \
    || bad "T2e: no coder spawn_agent_watchdog with bypass flag found"
}

# ===========================================================================
# TEST 3 — coder_brief does NOT contain the "no-change task is a valid
#           outcome" escape hatch phrasing
# ===========================================================================
test_coder_brief_no_noop_escape() {
  printf '\n=== AGENT-FLAGS TEST 3: coder_brief has no "no-change task is a valid outcome" ===\n'
  local home; home="$(mktemp -d "${TMPDIR:-/tmp}/agent-flags-test.XXXXXX")"
  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  local brief_out
  brief_out="$(run_coder_brief "$home" "$trimmed")"

  if printf '%s\n' "$brief_out" | grep -qiF "no-change task is a valid outcome"; then
    bad "T3: coder_brief still contains 'no-change task is a valid outcome'"
  else
    ok "T3: coder_brief does NOT contain 'no-change task is a valid outcome'"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 4 — coder_brief DOES contain "REAL IMPLEMENTATION TASK" framing
# ===========================================================================
test_coder_brief_real_implementation_framing() {
  printf '\n=== AGENT-FLAGS TEST 4: coder_brief has "real implementation task" framing ===\n'
  local home; home="$(mktemp -d "${TMPDIR:-/tmp}/agent-flags-test.XXXXXX")"
  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  local brief_out
  brief_out="$(run_coder_brief "$home" "$trimmed")"

  if printf '%s\n' "$brief_out" | grep -qiF "real implementation task"; then
    ok "T4: coder_brief contains 'real implementation task' framing"
  else
    bad "T4: coder_brief does NOT contain 'real implementation task' framing; first 5 lines: $(printf '%s\n' "$brief_out" | head -5)"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 5 — coder_brief DOES contain "UNIMPLEMENTED" (replacement escape hatch)
# ===========================================================================
test_coder_brief_unimplemented_language() {
  printf '\n=== AGENT-FLAGS TEST 5: coder_brief contains UNIMPLEMENTED language ===\n'
  local home; home="$(mktemp -d "${TMPDIR:-/tmp}/agent-flags-test.XXXXXX")"
  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  local brief_out
  brief_out="$(run_coder_brief "$home" "$trimmed")"

  if printf '%s\n' "$brief_out" | grep -qiF "unimplemented"; then
    ok "T5: coder_brief contains 'unimplemented' language for zero-change case"
  else
    bad "T5: coder_brief does NOT contain 'unimplemented' language; first 10 lines: $(printf '%s\n' "$brief_out" | head -10)"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 6 — Regression: all existing suites still pass
# ===========================================================================
test_regression_harvest() {
  printf '\n=== AGENT-FLAGS TEST 6a: harvest regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-harvest-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: harvest tests pass" \
    || bad "regression: harvest tests FAILED (rc=$rc)"
}

test_regression_resume() {
  printf '\n=== AGENT-FLAGS TEST 6b: resume regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-resume-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: resume tests pass" \
    || bad "regression: resume tests FAILED (rc=$rc)"
}

test_regression_m3() {
  printf '\n=== AGENT-FLAGS TEST 6c: M3 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: M3 tests pass" \
    || bad "regression: M3 tests FAILED (rc=$rc)"
}

test_regression_m4() {
  printf '\n=== AGENT-FLAGS TEST 6d: M4 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-m4-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: M4 tests pass" \
    || bad "regression: M4 tests FAILED (rc=$rc)"
}

test_regression_b1() {
  printf '\n=== AGENT-FLAGS TEST 6e: B1 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-b1-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: B1 tests pass" \
    || bad "regression: B1 tests FAILED (rc=$rc)"
}

test_regression_base() {
  printf '\n=== AGENT-FLAGS TEST 6f: base regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-base-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: base tests pass" \
    || bad "regression: base tests FAILED (rc=$rc)"
}

# ===========================================================================
# main
# ===========================================================================
main() {
  command -v jq      >/dev/null || { printf 'jq required\n'; exit 2; }
  command -v git     >/dev/null || { printf 'git required\n'; exit 2; }

  test_coder_has_bypass_flag
  test_reviewer_no_bypass_flag
  test_coder_brief_no_noop_escape
  test_coder_brief_real_implementation_framing
  test_coder_brief_unimplemented_language
  test_regression_harvest
  test_regression_resume
  test_regression_m3
  test_regression_m4
  test_regression_b1
  test_regression_base

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
