#!/usr/bin/env bash
#
# bench-matrix-base-test.sh — tests for the per-plan base override feature in
# bench-matrix.sh (Change 1 & 2 of the confirmatory-run harness update).
#
# Test plan:
#   T1: Override used in single-cell mode (--base <sha>): lookup_base_override
#       returns the override SHA; pick_base_sha is NOT consulted (run_cell's
#       dry-run logs "operator-supplied override" not "parent of first touching
#       commit").
#   T2: Override used in matrix mode (--bases P:sha,...): run_matrix dry-run
#       logs the override SHA per plan, and plans without overrides get the
#       pick_base_sha / HEAD path.
#   T3: Gate softened for override: verify_feature_absent with override=1 and
#       a pre-existing path does NOT abort (logs operator-asserted); override=0
#       with the same pre-existing path DOES hard-fail.
#   T4: No-override unchanged: pick_base_sha on a new-file feature resolves to
#       the commit BEFORE the file was added, and verify_feature_absent (no
#       override) passes cleanly for that base.
#   T5: Bad override SHA validation: supplying a non-existent SHA causes a
#       clear error exit.
#   T6: Regression — M3, M4, B1 test suites still pass.
#
# NO real claude invocations. Stub repos in temp dirs. Assertion counter +
# RESULTS line. Exits non-zero on any assertion failure.
#
# Usage: scripts/bench-matrix-base-test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLANAR_BIN="${PLANAR_BIN:-planar}"
PLANNING_CWD="$(git -C "$REPO_ROOT" worktree list --porcelain 2>/dev/null \
  | awk '/^worktree /{print $2; exit}')"
[ -n "$PLANNING_CWD" ] || PLANNING_CWD="$REPO_ROOT"

pass=0; fail=0
ok()  { printf 'PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad() { printf 'FAIL: %s\n' "$*"; fail=$((fail + 1)); }

# new_iso_home — create a fresh isolated BENCH_HOME, print its path.
new_iso_home() {
  mktemp -d "${TMPDIR:-/tmp}/base-test.XXXXXX"
}

# trimmed_matrix <dest> — copy the matrix with the final `main "$@"` stripped,
# so sourcing it does not auto-run. Prints the dest path.
trimmed_matrix() {
  local dest="$1"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$dest"
  printf '%s' "$dest"
}

# seed_plan <db> <cfg> <title> — plan + one task, no declared touches.
seed_plan() {
  local db="$1" cfg="$2" title="$3" pid
  pid="$(cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" plan create "$title" --scope global --json 2>/dev/null \
    | jq -r '.id')"
  ( cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" task add "task of $title" --plan "$pid" \
    --scope global >/dev/null 2>&1 )
  printf '%s' "$pid"
}

# ===========================================================================
# TEST 1 — single-cell --base override: lookup_base_override returns override,
#          and run_cell dry-run logs "operator-supplied override" not "parent
#          of first touching commit".
# ===========================================================================
# Strategy: build a stub corpus repo with a multi-commit history. Use --base
# to pin a mid-commit as override. Run --dry-run and confirm:
#   (a) the override SHA is logged as "operator-supplied override"
#   (b) "parent of first touching commit" (pick_base_sha path) is NOT logged
# Since we use a plan with NO declared touches (seed_plan), paths_list will be
# empty — the override is still validated and logged before the else-branch.
# We also directly test lookup_base_override to confirm it returns the SHA.
test_single_cell_base_override() {
  printf '\n=== BASE TEST 1: single-cell --base override wins over pick_base_sha ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  # Build a stub corpus repo.
  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t1@base-test.local
  git -C "$corpus" config user.name btest1
  printf 'v1\n' >"$corpus/feature.zig"
  git -C "$corpus" add feature.zig
  git -C "$corpus" commit -qm "root: create feature.zig"
  printf 'v2\n' >"$corpus/feature.zig"
  git -C "$corpus" add feature.zig
  git -C "$corpus" commit -qm "modify feature.zig"
  local override_sha; override_sha="$(git -C "$corpus" rev-parse HEAD)"
  printf 'other\n' >"$corpus/other.txt"
  git -C "$corpus" add other.txt
  git -C "$corpus" commit -qm "add other"

  # Plan with no declared touches (declared_paths will be empty).
  local pid; pid="$(seed_plan "$db" "$cfg" "override test")"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  # T1-direct: lookup_base_override returns the override SHA when --base is set.
  local direct_wrapper="$home/t1-direct.sh"
  cat >"$direct_wrapper" <<DIRECT
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
parse_args --plan "$pid" --arm strict --base "$override_sha" --dry-run
result="\$(lookup_base_override "$pid")"
printf '%s\n' "\$result"
DIRECT
  chmod +x "$direct_wrapper"
  local got_sha; got_sha="$(bash "$direct_wrapper" 2>/dev/null || true)"
  [ "$got_sha" = "$override_sha" ] \
    && ok "T1a: lookup_base_override returns override SHA (got '$got_sha')" \
    || bad "T1a: lookup_base_override returned '$got_sha' (expected '$override_sha')"

  # T1-dryrun: run_cell dry-run logs "operator-supplied override", NOT pick_base_sha.
  local wrapper="$home/t1-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
export BENCH_PLANNING_CWD="$PLANNING_CWD"
# shellcheck disable=SC1090
source "$trimmed"
PLANNING_CWD="$PLANNING_CWD"
LOG_ROOT="$home/cell-logs"
parse_args --plan "$pid" --arm strict --base "$override_sha" --dry-run
run_cell "$pid" strict 1
WRAP
  chmod +x "$wrapper"
  local out; out="$(bash "$wrapper" 2>&1 || true)"

  printf '%s\n' "$out" | grep -qF "operator-supplied override" \
    && ok "T1b: run_cell dry-run logs 'operator-supplied override'" \
    || bad "T1b: 'operator-supplied override' NOT found in output"

  printf '%s\n' "$out" | grep -qF "$override_sha" \
    && ok "T1c: override SHA '${override_sha}' present in dry-run output" \
    || bad "T1c: override SHA '${override_sha}' NOT in output"

  if printf '%s\n' "$out" | grep -qF "base_sha (parent of first touching commit)"; then
    bad "T1d: pick_base_sha log line appeared — override did NOT bypass it"
  else
    ok "T1d: pick_base_sha log NOT emitted (override bypassed it)"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 2 — matrix mode --bases override: dry-run shows override SHA per plan
# ===========================================================================
test_matrix_bases_override() {
  printf '\n=== BASE TEST 2: matrix --bases override per-plan ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t2@base-test.local
  git -C "$corpus" config user.name btest2
  printf 'init\n' >"$corpus/seed.txt"
  git -C "$corpus" add seed.txt
  git -C "$corpus" commit -qm base
  # A known SHA to use as override (HEAD before the second commit).
  local override_sha; override_sha="$(git -C "$corpus" rev-parse HEAD)"
  # One more commit so HEAD != override_sha.
  printf 'more\n' >"$corpus/seed.txt"
  git -C "$corpus" add seed.txt
  git -C "$corpus" commit -qm "second"

  # Two plans; plan 1 gets an override, plan 2 does not.
  local p1; p1="$(seed_plan "$db" "$cfg" "matrix override plan1")"
  local p2; p2="$(seed_plan "$db" "$cfg" "matrix override plan2")"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  local wrapper="$home/t2-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
export BENCH_PLANNING_CWD="$PLANNING_CWD"
# shellcheck disable=SC1090
source "$trimmed"
PLANNING_CWD="$PLANNING_CWD"
LOG_ROOT="$home/cell-logs"
require_tool sqlite3
# Matrix dry-run with override for p1 only.
parse_args --matrix --plans "${p1},${p2}" --reps 1 \
  --bases "${p1}:${override_sha}" --dry-run
run_matrix
WRAP
  chmod +x "$wrapper"
  local out; out="$(bash "$wrapper" 2>&1 || true)"

  # T2a: plan 1 shows override SHA and "operator-supplied override".
  printf '%s\n' "$out" | grep "plan ${p1}" | grep -qF "operator-supplied override" \
    && ok "T2a: plan ${p1} dry-run shows 'operator-supplied override'" \
    || bad "T2a: plan ${p1} dry-run did NOT show operator-supplied override; $(printf '%s\n' "$out" | grep "plan ${p1}" | head -3)"

  printf '%s\n' "$out" | grep "plan ${p1}" | grep -qF "$override_sha" \
    && ok "T2b: plan ${p1} dry-run shows override SHA ${override_sha}" \
    || bad "T2b: plan ${p1} did NOT show override SHA; $(printf '%s\n' "$out" | grep "plan ${p1}" | head -3)"

  # T2c: plan 2 (no override) shows corpus HEAD (the second commit).
  local head2; head2="$(git -C "$corpus" rev-parse HEAD)"
  printf '%s\n' "$out" | grep "plan ${p2}" | grep -qF "$head2" \
    && ok "T2c: plan ${p2} dry-run shows corpus HEAD (no override)" \
    || bad "T2c: plan ${p2} did NOT show corpus HEAD; $(printf '%s\n' "$out" | grep "plan ${p2}" | head -3)"

  # T2d: plan 2 must NOT show the plan-1 override SHA.
  if printf '%s\n' "$out" | grep "plan ${p2}" | grep -qF "$override_sha"; then
    bad "T2d: plan ${p2} picked up override SHA that belongs to plan ${p1}"
  else
    ok "T2d: plan ${p2} correctly did NOT use plan ${p1}'s override SHA"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 3 — Gate softened for override vs hard-fails for non-override.
# Directly tests verify_feature_absent with override=1 (soft) and override=0
# (hard). Does not go through run_cell (which needs planar plan/task reads).
# ===========================================================================
test_gate_softened_for_override() {
  printf '\n=== BASE TEST 3: existence gate softened under override, hard-fail without ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t3@base-test.local
  git -C "$corpus" config user.name btest3
  # Root commit: pre-existing file (declared path exists at the very start).
  printf 'original\n' >"$corpus/existing.zig"
  git -C "$corpus" add existing.zig
  git -C "$corpus" commit -qm "root: existing.zig present"
  local root_sha; root_sha="$(git -C "$corpus" rev-parse HEAD)"
  # Second commit: modifies the file.
  printf 'modified\n' >"$corpus/existing.zig"
  git -C "$corpus" add existing.zig
  git -C "$corpus" commit -qm "modify existing.zig"
  local second_sha; second_sha="$(git -C "$corpus" rev-parse HEAD)"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  # ------ Sub-case A: override=1 → softened (no abort), even though path exists ------
  local wrapA="$home/t3a-wrapper.sh"
  cat >"$wrapA" <<WRAPA
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
# Call verify_feature_absent with override=1 (operator-asserted path).
# existing.zig exists at root_sha — should NOT abort.
verify_feature_absent "$corpus" "$root_sha" 1 "existing.zig"
printf 'completed\n'
WRAPA
  chmod +x "$wrapA"
  local rcA=0
  local outA; outA="$(bash "$wrapA" 2>&1)" || rcA=$?

  # Must NOT abort.
  [ "$rcA" -eq 0 ] \
    && ok "T3a: override=1 + pre-existing path: verify_feature_absent did NOT abort (rc=0)" \
    || bad "T3a: override=1 + pre-existing path: verify_feature_absent ABORTED (rc=$rcA)"

  # Must emit the operator-asserted message.
  printf '%s\n' "$outA" | grep -qiF "operator-asserted" \
    && ok "T3b: gate logged 'operator-asserted' for pre-existing path under override" \
    || bad "T3b: 'operator-asserted' NOT found; outA=${outA}"

  # Must also log that the path pre-exists (INFO signal, not abort).
  printf '%s\n' "$outA" | grep -qiF "existing.zig" \
    && ok "T3c: pre-existing path name logged (INFO, not abort)" \
    || bad "T3c: path name NOT logged; outA=${outA}"

  # Must NOT say "gate FAILED".
  if printf '%s\n' "$outA" | grep -qiF "gate FAILED"; then
    bad "T3d: 'gate FAILED' appeared in override output (should be suppressed)"
  else
    ok "T3d: 'gate FAILED' correctly absent in override path"
  fi

  # ------ Sub-case B: override=0 → hard-fail because path exists ------
  local wrapB="$home/t3b-wrapper.sh"
  cat >"$wrapB" <<WRAPB
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
# Call verify_feature_absent with override=0 (pick_base_sha path).
# existing.zig exists at second_sha — must abort.
verify_feature_absent "$corpus" "$second_sha" 0 "existing.zig"
WRAPB
  chmod +x "$wrapB"
  local rcB=0
  local outB; outB="$(bash "$wrapB" 2>&1)" || rcB=$?

  [ "$rcB" -ne 0 ] \
    && ok "T3e: override=0 + pre-existing path: verify_feature_absent hard-failed (rc=$rcB)" \
    || bad "T3e: override=0 + pre-existing path: verify_feature_absent did NOT fail (rc=$rcB)"

  printf '%s\n' "$outB" | grep -qiF "gate FAILED" \
    && ok "T3f: 'gate FAILED' in no-override output" \
    || bad "T3f: 'gate FAILED' NOT found; outB=${outB}"

  rm -rf "$home"
}

# ===========================================================================
# TEST 4 — No-override unchanged: pick_base_sha + gate OK for a new-file feature.
# Directly tests pick_base_sha and verify_feature_absent with override=0 for
# a path that was NEW (not present at the picked base).
# ===========================================================================
test_no_override_unchanged() {
  printf '\n=== BASE TEST 4: no-override path unchanged (pick_base_sha + gate OK) ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t4@base-test.local
  git -C "$corpus" config user.name btest4
  # Commit 0: bootstrap (no declared path yet).
  printf 'bootstrap\n' >"$corpus/bootstrap.txt"
  git -C "$corpus" add bootstrap.txt
  git -C "$corpus" commit -qm "bootstrap"
  local bootstrap_sha; bootstrap_sha="$(git -C "$corpus" rev-parse HEAD)"
  # Commit 1: creates the declared path (NEW file).
  printf 'new feature\n' >"$corpus/newfeature.zig"
  git -C "$corpus" add newfeature.zig
  git -C "$corpus" commit -qm "add newfeature.zig"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  # T4-pick: pick_base_sha("newfeature.zig") must return bootstrap_sha.
  local pick_wrapper="$home/t4-pick.sh"
  cat >"$pick_wrapper" <<PICK
#!/usr/bin/env bash
set -euo pipefail
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
result="\$(pick_base_sha "$corpus" "newfeature.zig")"
printf '%s\n' "\$result"
PICK
  chmod +x "$pick_wrapper"
  local picked; picked="$(bash "$pick_wrapper" 2>/dev/null || true)"

  [ "$picked" = "$bootstrap_sha" ] \
    && ok "T4a: pick_base_sha resolves to pre-feature commit (${bootstrap_sha})" \
    || bad "T4a: pick_base_sha returned '${picked}' (expected '${bootstrap_sha}')"

  # T4-gate: verify_feature_absent with override=0 at bootstrap_sha must pass
  # (newfeature.zig does NOT exist at bootstrap).
  local gate_wrapper="$home/t4-gate.sh"
  cat >"$gate_wrapper" <<GATE
#!/usr/bin/env bash
set -euo pipefail
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
verify_feature_absent "$corpus" "$bootstrap_sha" 0 "newfeature.zig"
printf 'gate-ok\n'
GATE
  chmod +x "$gate_wrapper"
  local gate_rc=0
  local gate_out; gate_out="$(bash "$gate_wrapper" 2>&1)" || gate_rc=$?

  [ "$gate_rc" -eq 0 ] \
    && ok "T4b: verify_feature_absent passes for new-file at pick_base_sha (rc=0)" \
    || bad "T4b: verify_feature_absent FAILED for new-file at pick_base_sha (rc=$gate_rc)"

  printf '%s\n' "$gate_out" | grep -qF "gate OK" \
    && ok "T4c: gate logged 'gate OK' for new-file feature" \
    || bad "T4c: 'gate OK' NOT found; out=${gate_out}"

  if printf '%s\n' "$gate_out" | grep -qiF "gate FAILED"; then
    bad "T4d: 'gate FAILED' in output for a new-file feature (should pass)"
  else
    ok "T4d: 'gate FAILED' absent — correct for new-file feature"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 5 — Bad override SHA causes clear validation error.
# ===========================================================================
test_bad_override_sha() {
  printf '\n=== BASE TEST 5: bad override SHA causes validation error ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t5@base-test.local
  git -C "$corpus" config user.name btest5
  printf 'seed\n' >"$corpus/seed.txt"
  git -C "$corpus" add seed.txt
  git -C "$corpus" commit -qm base

  local pid; pid="$(seed_plan "$db" "$cfg" "bad-sha test")"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  # T5a: --base with non-existent SHA causes non-zero exit from lookup_base_override.
  local wrapA="$home/t5a-wrapper.sh"
  cat >"$wrapA" <<WRAPA
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
parse_args --plan "$pid" --arm strict --base "0000000000000000000000000000000000000000" --dry-run
lookup_base_override "$pid"
WRAPA
  chmod +x "$wrapA"
  local rcA=0
  bash "$wrapA" 2>/dev/null || rcA=$?
  [ "$rcA" -ne 0 ] \
    && ok "T5a: bad --base SHA causes non-zero exit (rc=$rcA)" \
    || bad "T5a: bad --base SHA did NOT cause error exit (rc=$rcA)"

  # T5b: --bases with non-existent SHA for the given plan.
  local wrapB="$home/t5b-wrapper.sh"
  cat >"$wrapB" <<WRAPB
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
parse_args --matrix --plans "$pid" --reps 1 \
  --bases "${pid}:deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" --dry-run
lookup_base_override "$pid"
WRAPB
  chmod +x "$wrapB"
  local rcB=0
  bash "$wrapB" 2>/dev/null || rcB=$?
  [ "$rcB" -ne 0 ] \
    && ok "T5b: bad --bases SHA causes non-zero exit (rc=$rcB)" \
    || bad "T5b: bad --bases SHA did NOT cause error exit (rc=$rcB)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 6 — Regression: M3, M4, B1 still pass
# ===========================================================================
test_regression_m3() {
  printf '\n=== BASE TEST 6a: M3 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: M3 tests pass" \
    || bad "regression: M3 tests FAILED (rc=$rc)"
}

test_regression_m4() {
  printf '\n=== BASE TEST 6b: M4 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-m4-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: M4 tests pass" \
    || bad "regression: M4 tests FAILED (rc=$rc)"
}

test_regression_b1() {
  printf '\n=== BASE TEST 6c: B1 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-b1-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: B1 tests pass" \
    || bad "regression: B1 tests FAILED (rc=$rc)"
}

# ===========================================================================
# main
# ===========================================================================
main() {
  command -v jq      >/dev/null || { printf 'jq required\n'; exit 2; }
  command -v sqlite3 >/dev/null || { printf 'sqlite3 required\n'; exit 2; }
  command -v git     >/dev/null || { printf 'git required\n'; exit 2; }

  test_single_cell_base_override
  test_matrix_bases_override
  test_gate_softened_for_override
  test_no_override_unchanged
  test_bad_override_sha
  test_regression_m3
  test_regression_m4
  test_regression_b1

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
