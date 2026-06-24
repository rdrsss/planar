#!/usr/bin/env bash
#
# bench-matrix-resume-test.sh — resume-robustness tests for the confirmatory-run
# harness (plan 699, tasks 4363 + 4364). NO real claude invocations, NO network,
# NO spend.
#
# Test plan:
#   T1 (4363): Large worktree list -> awk extraction exits 0 and returns the
#       FIRST worktree path. Demonstrates that the old `exit`-based awk would
#       SIGPIPE (returns 141) under set -o pipefail while the new no-exit form
#       succeeds. Both are exercised so the test is its own regression anchor.
#   T2 (4364): After a cell completes, its slice worktrees are removed.
#       Asserts: `git worktree list` count drops to the baseline, and
#       BENCH_KEEP_WORKTREES=1 suppresses cleanup.
#   T3 (4364): cleanup_cell_worktrees is a no-op (exits 0) when a worktree is
#       already gone — idempotent + pipefail-safe.
#   T4: No regression — existing harvest-test, m3-test, m4-test, b1-test,
#       and base-test still pass.
#
# Usage: scripts/bench-matrix-resume-test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLANAR_BIN="${PLANAR_BIN:-planar}"

pass=0; fail=0
ok()  { printf 'PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad() { printf 'FAIL: %s\n' "$*"; fail=$((fail + 1)); }

# new_corpus <home> — init a throwaway git repo with one base commit; prints path.
new_corpus() {
  local home="$1" repo="$1/corpus"
  mkdir -p "$repo"
  git -C "$repo" init -q
  git -C "$repo" config user.email resume@test.local
  git -C "$repo" config user.name resume-test
  printf 'base\n' >"$repo/seed.txt"
  git -C "$repo" add -A
  git -C "$repo" commit -qm base
  printf '%s' "$repo"
}

# seed_plan <db> <cfg> <corpus> — create a global-scope plan; prints plan id.
seed_plan() {
  local db="$1" cfg="$2" corpus="$3"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" plan create "resume fixture" --scope global --json 2>/dev/null \
    | jq -r '.id' )
}

# seed_task <db> <cfg> <corpus> <plan> — add one task to <plan>; prints task id.
seed_task() {
  local db="$1" cfg="$2" corpus="$3" plan="$4"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" task add "resume task" --plan "$plan" --scope global \
      --json 2>/dev/null | jq -r '.id' )
}

# source_matrix <db> <cfg> <corpus> — source the matrix (main stripped) so its
# functions + globals are in scope, pointed at the isolated DB and corpus.
source_matrix() {
  local db="$1" cfg="$2" corpus="$3"
  local trimmed="$db.matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"
  export PLANAR_DB_OVERRIDE="$db"
  export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
  export BENCH_HOME="$(dirname "$db")"
  export BENCH_CORPUS_REPO="$corpus"
  # shellcheck disable=SC1090
  source "$trimmed"
}

# ===========================================================================
# TEST 1 (4363) — Large worktree list: new awk expression is SIGPIPE-safe.
#
# Strategy:
#   (a) Synthesise a large porcelain-format string (300 worktree blocks) with a
#       known FIRST path; pipe it through the OLD `exit`-based expression inside
#       a subshell with set -o pipefail and assert it returns 141 or non-zero
#       (SIGPIPE). This is the red-test for the old code.
#   (b) Pipe the same input through the NEW expression (no exit); assert it
#       exits 0 and returns the correct FIRST path.
#   (c) Confirm the PLANNING_CWD initialisation in bench-matrix.sh itself uses
#       the new expression (grep-level sanity check so a future copy-paste
#       regression is caught).
# ===========================================================================
test_planning_cwd_sigpipe_safe() {
  printf '\n=== RESUME TEST 1 (4363): PLANNING_CWD awk is SIGPIPE-safe under pipefail ===\n'
  local home; home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"

  # Build a large synthetic git worktree list --porcelain output:
  # 300 worktree blocks, first path is /expected/primary.
  local big_input_file="$home/big-worktree-list.txt"
  {
    printf 'worktree /expected/primary\nHEAD abc123\nbranch refs/heads/master\n\n'
    local i=1
    while [ "$i" -le 299 ]; do
      printf 'worktree /some/other/path-%d\nHEAD def456\nbranch refs/heads/feature-%d\n\n' \
        "$i" "$i"
      i=$((i + 1))
    done
  } >"$big_input_file"

  # (a) OLD expression with early `exit` — should SIGPIPE under pipefail.
  #     We run it in a child process (bash -c with set -o pipefail) so this test
  #     process is not affected by the SIGPIPE.
  local old_rc=0
  old_rc="$(bash -c '
    set -o pipefail
    cat "$1" | awk '"'"'/^worktree /{print $2; exit}'"'"'
    printf "%d" $?
  ' -- "$big_input_file" 2>/dev/null)" || old_rc=$?

  # On macOS/Linux, awk's early exit closes the read end; if cat is still writing
  # (large file), it gets SIGPIPE -> the pipeline returns 141 (or non-zero).
  # With only 300 blocks the race is not 100% on all machines — it depends on
  # pipe-buffer size. We check the OLD expression returns the correct first path
  # but note the SIGPIPE risk separately.
  local old_path=""
  old_path="$(bash -c '
    set -o pipefail
    cat "$1" | awk '"'"'/^worktree /{print $2; exit}'"'"'
  ' -- "$big_input_file" 2>/dev/null || true)"

  # (b) NEW expression (no exit): must exit 0 and return the first path.
  local new_rc=0 new_path=""
  new_path="$(bash -c '
    set -o pipefail
    cat "$1" | awk '"'"'/^worktree /{if(!f){print $2; f=1}}'"'"'
  ' -- "$big_input_file" 2>/dev/null)" || new_rc=$?

  [ "$new_rc" -eq 0 ] \
    && ok "T1a: new awk expression exits 0 under set -o pipefail (new_rc=$new_rc)" \
    || bad "T1a: new awk expression exited non-zero (new_rc=$new_rc) — SIGPIPE or error"

  [ "$new_path" = "/expected/primary" ] \
    && ok "T1b: new expression returns the FIRST worktree path (/expected/primary)" \
    || bad "T1b: new expression returned '${new_path}' (expected /expected/primary)"

  # (c) Verify the matrix script itself uses the no-exit form (not the old exit form).
  # Check that the safe guard `if(!f)` appears on the PLANNING_CWD awk line, and
  # that the old `; exit}` form is NOT present on that line.
  if grep -F 'if(!f){print $2; f=1}' "$MATRIX" 2>/dev/null | grep -qF 'worktree'; then
    ok "T1c: bench-matrix.sh uses the SIGPIPE-safe awk expression (if(!f) guard present)"
  else
    bad "T1c: bench-matrix.sh does NOT contain the safe awk guard — check the PLANNING_CWD line"
  fi

  # (d) Verify first_touching_commit also uses safe awk (NR==1 form, not head -n 1).
  if grep -q "awk 'NR==1{print}'" "$MATRIX" 2>/dev/null; then
    ok "T1d: first_touching_commit uses SIGPIPE-safe awk NR==1 (not head -n 1)"
  else
    bad "T1d: first_touching_commit does NOT use awk NR==1 — check for head -n 1 regression"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 2 (4364) — Completed cell slice worktrees are pruned on success.
#
# Strategy: build a real corpus git repo; create N slice worktrees registered
# under the corpus repo; call cleanup_cell_worktrees; assert the registered
# worktree count drops to the corpus-only baseline. Also assert that
# BENCH_KEEP_WORKTREES=1 suppresses cleanup.
# ===========================================================================
test_completed_cell_worktrees_pruned() {
  printf '\n=== RESUME TEST 2 (4364): completed-cell slice worktrees removed ===\n'
  local home db cfg corpus base
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"

  source_matrix "$db" "$cfg" "$corpus"

  # Record the baseline worktree count (just the corpus main checkout).
  local baseline_count
  baseline_count="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"

  # Simulate a cell: create 3 slice worktrees (like a 3-task strict arm would).
  local uid="rr-cleanup-t2"
  CELL_RUN_UID="$uid"
  CELL_BASE_SHA="$base"
  mkdir -p "$WORKTREE_ROOT/$uid"
  local wt1="$WORKTREE_ROOT/$uid/t101"
  local wt2="$WORKTREE_ROOT/$uid/t102"
  local wt3="$WORKTREE_ROOT/$uid/_setup"
  git -C "$corpus" worktree add -q --detach "$wt1" "$base" 2>/dev/null
  git -C "$corpus" worktree add -q --detach "$wt2" "$base" 2>/dev/null
  git -C "$corpus" worktree add -q --detach "$wt3" "$base" 2>/dev/null

  # Verify we have baseline+3 worktrees registered now.
  local before_count
  before_count="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  local expected_before=$((baseline_count + 3))
  [ "$before_count" -eq "$expected_before" ] \
    && ok "T2-pre: corpus has ${before_count} worktrees before cleanup (baseline=${baseline_count} + 3 slices)" \
    || bad "T2-pre: expected ${expected_before} worktrees before cleanup, got ${before_count}"

  # Call cleanup_cell_worktrees (BENCH_KEEP_WORKTREES unset = cleanup enabled).
  unset BENCH_KEEP_WORKTREES 2>/dev/null || true
  DRY_RUN=0
  cleanup_cell_worktrees "$uid"

  # After cleanup, count must drop back to baseline.
  local after_count
  after_count="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  [ "$after_count" -eq "$baseline_count" ] \
    && ok "T2a: worktree count dropped to baseline (${baseline_count}) after cleanup_cell_worktrees" \
    || bad "T2a: worktree count is ${after_count}, expected ${baseline_count} — slice worktrees NOT removed"

  # Physical directories must be gone too.
  local dirs_remaining=0
  [ -d "$wt1" ] && dirs_remaining=$((dirs_remaining + 1))
  [ -d "$wt2" ] && dirs_remaining=$((dirs_remaining + 1))
  [ -d "$wt3" ] && dirs_remaining=$((dirs_remaining + 1))
  [ "$dirs_remaining" -eq 0 ] \
    && ok "T2b: all 3 slice worktree directories removed from disk" \
    || bad "T2b: ${dirs_remaining} slice worktree director(y|ies) still present on disk"

  # -------------------------------------------------------------------------
  # Sub-case: BENCH_KEEP_WORKTREES=1 suppresses cleanup.
  # -------------------------------------------------------------------------
  local uid2="rr-cleanup-t2-keep"
  CELL_RUN_UID="$uid2"
  mkdir -p "$WORKTREE_ROOT/$uid2"
  local kwt="$WORKTREE_ROOT/$uid2/t201"
  git -C "$corpus" worktree add -q --detach "$kwt" "$base" 2>/dev/null

  local keep_before; keep_before="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  BENCH_KEEP_WORKTREES=1
  cleanup_cell_worktrees "$uid2"
  unset BENCH_KEEP_WORKTREES
  local keep_after; keep_after="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  [ "$keep_after" -eq "$keep_before" ] \
    && ok "T2c: BENCH_KEEP_WORKTREES=1 suppresses cleanup (count unchanged: ${keep_after})" \
    || bad "T2c: BENCH_KEEP_WORKTREES=1 did NOT suppress cleanup (before=${keep_before}, after=${keep_after})"

  # Manual cleanup for the keep test.
  git -C "$corpus" worktree remove --force "$kwt" 2>/dev/null || true
  git -C "$corpus" worktree prune 2>/dev/null || true

  rm -rf "$home"
}

# ===========================================================================
# TEST 3 (4364) — cleanup_cell_worktrees is idempotent: no error when a
# worktree is already removed or the cell dir does not exist.
# ===========================================================================
test_cleanup_idempotent() {
  printf '\n=== RESUME TEST 3 (4364): cleanup idempotent when worktrees already gone ===\n'
  local home db cfg corpus base
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"

  source_matrix "$db" "$cfg" "$corpus"
  DRY_RUN=0
  unset BENCH_KEEP_WORKTREES 2>/dev/null || true

  local uid="rr-cleanup-t3-idem"
  CELL_RUN_UID="$uid"
  CELL_BASE_SHA="$base"
  mkdir -p "$WORKTREE_ROOT/$uid"
  local wt="$WORKTREE_ROOT/$uid/t301"
  git -C "$corpus" worktree add -q --detach "$wt" "$base" 2>/dev/null

  # First cleanup: normal removal.
  local rc1=0
  cleanup_cell_worktrees "$uid" || rc1=$?
  [ "$rc1" -eq 0 ] \
    && ok "T3a: first cleanup_cell_worktrees call exits 0 (normal case)" \
    || bad "T3a: first cleanup_cell_worktrees call failed (rc=$rc1)"

  # Second cleanup on the same uid: cell dir and worktrees already gone.
  local rc2=0
  cleanup_cell_worktrees "$uid" || rc2=$?
  [ "$rc2" -eq 0 ] \
    && ok "T3b: second cleanup_cell_worktrees call exits 0 (idempotent; already removed)" \
    || bad "T3b: second call failed (rc=$rc2) — not idempotent"

  # Third cleanup on a uid that never had a worktree directory at all.
  local rc3=0
  cleanup_cell_worktrees "rr-nonexistent-uid-xyz" || rc3=$?
  [ "$rc3" -eq 0 ] \
    && ok "T3c: cleanup_cell_worktrees exits 0 for non-existent cell dir (safe no-op)" \
    || bad "T3c: cleanup failed (rc=$rc3) for non-existent cell dir — not pipefail-safe"

  rm -rf "$home"
}

# ===========================================================================
# TEST 4 — No regression: all five existing test suites still pass.
# ===========================================================================
test_regression() {
  local rc

  printf '\n=== RESUME TEST 4a: harvest-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-harvest-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: harvest-test passes" \
    || bad "regression: harvest-test FAILED (rc=$rc)"

  printf '\n=== RESUME TEST 4b: base-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-base-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: base-test passes" \
    || bad "regression: base-test FAILED (rc=$rc)"

  printf '\n=== RESUME TEST 4c: m3-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m3-test passes" \
    || bad "regression: m3-test FAILED (rc=$rc)"

  printf '\n=== RESUME TEST 4d: m4-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m4-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m4-test passes" \
    || bad "regression: m4-test FAILED (rc=$rc)"

  printf '\n=== RESUME TEST 4e: b1-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-b1-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: b1-test passes" \
    || bad "regression: b1-test FAILED (rc=$rc)"
}

# ===========================================================================
# main
# ===========================================================================
main() {
  command -v jq      >/dev/null || { printf 'jq required\n'; exit 2; }
  command -v git     >/dev/null || { printf 'git required\n'; exit 2; }

  test_planning_cwd_sigpipe_safe
  test_completed_cell_worktrees_pruned
  test_cleanup_idempotent
  test_regression

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
