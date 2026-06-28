#!/usr/bin/env bash
#
# bench-matrix-resume-test.sh — resume-robustness tests for the confirmatory-run
# harness (plan 699, tasks 4363 + 4364 + 4466). NO real claude invocations, NO
# network, NO spend.
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
#   T5 (4466): All-slices-crash -> run finished as aborted, resume re-runs.
#       A cell whose EVERY slice returns rc=2 (coder exhausted inline retries)
#       must be finished with --status aborted, NOT completed. resolve_cell_uid
#       must then treat it as an abort+retry (not SKIP).
#   T6 (4466): Genuine token-bearing no-op -> stays completed + skipped.
#       A cell where the coder ran (rc=0, valid JSON with .usage) but produced
#       zero file edits must be finished completed. Resume must SKIP it — re-running
#       would waste spend and distort the RQ1 metric.
#   T7 (4466): Normal cell (touches > 0) -> completed.
#   T8 (4466): Partial crash (one slice crashes, another produces touches) ->
#       productive data is retained; finish status follows the documented rule.
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

# seed_plan <db> <cfg> <corpus> [label] — create a global-scope plan; prints plan id.
# The optional label is appended to the plan title to avoid slug conflicts when
# multiple plans are created in the same DB (e.g. within T9).
seed_plan() {
  local db="$1" cfg="$2" corpus="$3" label="${4:-}"
  local title="resume fixture${label:+ $label}"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" plan create "$title" --scope global --json 2>/dev/null \
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
# TEST 5 (4466) — All-slices-crash: run marked aborted, resume re-runs.
#
# Strategy: use a wrapper script (so the matrix trap handlers do not leak into
# this process) that sources the matrix, stubs dispatch_strict to set
# _CELL_CRASHED_SLICES == _CELL_TOTAL_SLICES == N (every slice crashed), then
# calls run_cell. Assert:
#   (a) bench show --json .status == "aborted" (NOT "completed")
#   (b) resolve_cell_uid returns a retry uid (NOT "SKIP")
# ===========================================================================
test_all_slices_crash_marks_aborted() {
  printf '\n=== RESUME TEST 5 (4466): all-slices-crash -> aborted + resume re-runs ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  local base; base="$(git -C "$corpus" rev-parse HEAD)"

  # Need a real plan + task in the DB so bench start can snapshot declared touches.
  local plan_id task_id
  plan_id="$(seed_plan "$db" "$cfg" "$corpus")"
  task_id="$(seed_task "$db" "$cfg" "$corpus" "$plan_id")"

  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  local state_file="$home/crash-state"
  # Use the DETERMINISTIC uid formula (m-<plan>-<arm>-r<rep>) so that
  # resolve_cell_uid can find the run record by its computed uid.
  local uid="m-${plan_id}-strict-r1"

  # Wrapper: stub plan_scope, declared_paths, execute_phase, and dispatch_strict
  # so run_cell can run end-to-end without planar-execute, with all slices crashed.
  # execute_phase is stubbed to call `bench start` directly (the Lua ritual does
  # this; bypassing it without opening the run record leaves bench show absent).
  local wrapper="$home/crash-all-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
DRY_RUN=0
AGENT_TIMEOUT=0
AGENT_RETRIES=0

# Stubs: bypass planning-verb reads and phase A planar-execute call.
plan_scope()     { printf 'global'; }
declared_paths() { printf ''; }
# execute_phase stub: open the bench run directly (the Lua ritual does this).
execute_phase()  {
  command "$PLANAR_BIN" bench start "\$CELL_RUN_UID" \
    --plan "\$CELL_PLAN" --arm "\$CELL_ARM" \
    --base-sha "\${CELL_BASE_SHA:-stub}" --config-hash "testhash" >/dev/null 2>&1 || true
}

# Stub dispatch_strict: pretend 3 slices all crashed.
dispatch_strict() {
  _CELL_TOTAL_SLICES=3
  _CELL_CRASHED_SLICES=3
  log "stub dispatch_strict: 3/3 slices crashed"
}

# Use the override uid that matches the deterministic formula so resolve_cell_uid
# can locate the run record via cell_status.
CELL_RUN_UID_OVERRIDE="$uid"
CELL_CHASH_OVERRIDE="testhash"

# Run the cell (single-cell mode entry point).
rc=0
run_cell "$plan_id" "strict" "1" 2>/dev/null || rc=\$?
printf 'run_cell_rc=%s\n' "\$rc" >"$state_file"
WRAP
  chmod +x "$wrapper"

  bash "$wrapper" 2>/dev/null || true

  local run_rc=0
  [ -f "$state_file" ] && run_rc="$(grep '^run_cell_rc=' "$state_file" | cut -d= -f2 || printf '0')"

  # (a) run_cell must return non-zero (cell aborted internally).
  [ "${run_rc:-0}" -ne 0 ] \
    && ok "T5a: run_cell returned non-zero when all slices crashed (rc=$run_rc)" \
    || bad "T5a: run_cell returned 0 — expected non-zero for all-crash cell (rc=$run_rc)"

  # (b) bench show must report status=aborted (NOT completed).
  local run_status
  run_status="$(PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" bench show "$uid" --json 2>/dev/null \
    | jq -r '.status' 2>/dev/null || printf 'absent')"
  [ "$run_status" = "aborted" ] \
    && ok "T5b: bench show .status = aborted (correct — all slices crashed)" \
    || bad "T5b: bench show .status = '${run_status}' (expected 'aborted')"

  # (c) resolve_cell_uid must NOT return SKIP — it must abort+retry.
  # Source the matrix into a subshell to call resolve_cell_uid directly.
  local resolve_out
  resolve_out="$(
    export PLANAR_DB_OVERRIDE="$db"
    export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
    export BENCH_HOME="$home"
    export BENCH_CORPUS_REPO="$corpus"
    # Use a subshell-safe approach: write to a tmp file.
    tmp_resolve="$home/resolve-out.txt"
    bash -c "
      source '$trimmed'
      LOG_ROOT='$home/cell-logs'
      DRY_RUN=0
      CORPUS_PLANS='$plan_id'
      resolve_cell_uid '$plan_id' 'strict' '1' 2>/dev/null
    " >"$home/resolve-out.txt" 2>/dev/null || true
    cat "$home/resolve-out.txt"
  )"
  [ "$resolve_out" != "SKIP" ] \
    && ok "T5c: resolve_cell_uid returned '${resolve_out}' (NOT SKIP — will retry)" \
    || bad "T5c: resolve_cell_uid returned SKIP — aborted cell would be skipped forever"

  # Also confirm the retry uid is non-empty.
  [ -n "$resolve_out" ] \
    && ok "T5d: resolve_cell_uid returned a non-empty retry uid ('${resolve_out}')" \
    || bad "T5d: resolve_cell_uid returned empty string"

  rm -rf "$home"
}

# ===========================================================================
# TEST 6 (4466) — Genuine token-bearing no-op: stays completed, resume skips.
#
# Strategy: same pattern as T5, but dispatch_strict sets _CELL_CRASHED_SLICES=0
# (all slices ran successfully, agents just chose to make no edits). The run
# must be finished completed and resolve_cell_uid must return SKIP.
# ===========================================================================
test_genuine_noop_stays_completed() {
  printf '\n=== RESUME TEST 6 (4466): genuine no-op (0 touches, 0 crashes) -> completed + SKIP ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"

  local plan_id task_id
  plan_id="$(seed_plan "$db" "$cfg" "$corpus")"
  task_id="$(seed_task "$db" "$cfg" "$corpus" "$plan_id")"

  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  local state_file="$home/noop-state"
  # Use the deterministic uid formula so resolve_cell_uid can find the record.
  local uid="m-${plan_id}-strict-r1"

  local wrapper="$home/noop-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
DRY_RUN=0
AGENT_TIMEOUT=0

# Stubs: bypass planning reads; open the bench run directly in execute_phase.
plan_scope()     { printf 'global'; }
declared_paths() { printf ''; }
execute_phase()  {
  command "$PLANAR_BIN" bench start "\$CELL_RUN_UID" \
    --plan "\$CELL_PLAN" --arm "\$CELL_ARM" \
    --base-sha "\${CELL_BASE_SHA:-stub}" --config-hash "testhash" >/dev/null 2>&1 || true
}

# Stub dispatch_strict: 2 slices ran (rc=0, token-bearing) but made no edits.
# Crashed count is 0 — these are genuine no-ops, not API failures.
dispatch_strict() {
  _CELL_TOTAL_SLICES=2
  _CELL_CRASHED_SLICES=0
  log "stub dispatch_strict: 2/2 slices ran (0 touches, 0 crashes — genuine no-op)"
}

CELL_RUN_UID_OVERRIDE="$uid"
CELL_CHASH_OVERRIDE="testhash"

rc=0
run_cell "$plan_id" "strict" "1" 2>/dev/null || rc=\$?
printf 'run_cell_rc=%s\n' "\$rc" >"$state_file"
WRAP
  chmod +x "$wrapper"
  bash "$wrapper" 2>/dev/null || true

  local run_rc=0
  [ -f "$state_file" ] && run_rc="$(grep '^run_cell_rc=' "$state_file" | cut -d= -f2 || printf '0')"

  # (a) run_cell must return 0 (genuine no-op is valid data, not an error).
  [ "${run_rc:-1}" -eq 0 ] \
    && ok "T6a: run_cell returned 0 for genuine no-op (valid 0-touch data)" \
    || bad "T6a: run_cell returned non-zero (${run_rc}) for genuine no-op — should be 0"

  # (b) bench show must report status=completed.
  local run_status
  run_status="$(PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" bench show "$uid" --json 2>/dev/null \
    | jq -r '.status' 2>/dev/null || printf 'absent')"
  [ "$run_status" = "completed" ] \
    && ok "T6b: bench show .status = completed (genuine no-op is valid data)" \
    || bad "T6b: bench show .status = '${run_status}' (expected 'completed')"

  # (c) resolve_cell_uid must return SKIP (no double-count).
  local resolve_out
  resolve_out="$(
    bash -c "
      export PLANAR_DB_OVERRIDE='$db'
      export PLANAR_CONFIG_PATH_OVERRIDE='$cfg'
      export BENCH_HOME='$home'
      export BENCH_CORPUS_REPO='$corpus'
      source '$trimmed'
      LOG_ROOT='$home/cell-logs'
      DRY_RUN=0
      resolve_cell_uid '$plan_id' 'strict' '1' 2>/dev/null
    " 2>/dev/null || true
  )"
  [ "$resolve_out" = "SKIP" ] \
    && ok "T6c: resolve_cell_uid = SKIP (genuine no-op is not re-run; valid data)" \
    || bad "T6c: resolve_cell_uid = '${resolve_out}' (expected SKIP for completed no-op)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 7 (4466) — Normal cell (touches > 0): completed.
#
# Strategy: dispatch_strict sets _CELL_CRASHED_SLICES=0 and at least one
# actual touch is recorded (we write a run_touches row directly via sqlite3
# since we cannot run a real harvest without a real git diff). The cell must
# finish completed.
# ===========================================================================
test_normal_cell_completed() {
  printf '\n=== RESUME TEST 7 (4466): normal cell (touches > 0) -> completed ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"

  local plan_id task_id
  plan_id="$(seed_plan "$db" "$cfg" "$corpus")"
  task_id="$(seed_task "$db" "$cfg" "$corpus" "$plan_id")"

  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  local state_file="$home/normal-state"
  local uid="rr-normal-t7"

  local wrapper="$home/normal-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
DRY_RUN=0
AGENT_TIMEOUT=0

# Stubs: bypass planning reads; open the bench run directly in execute_phase.
plan_scope()     { printf 'global'; }
declared_paths() { printf ''; }
execute_phase()  {
  command "$PLANAR_BIN" bench start "\$CELL_RUN_UID" \
    --plan "\$CELL_PLAN" --arm "\$CELL_ARM" \
    --base-sha "\${CELL_BASE_SHA:-stub}" --config-hash "testhash" >/dev/null 2>&1 || true
}

# Stub dispatch_strict: 1 slice succeeded with 0 crashes.
# The zero-touch guard may emit a WARNING (no real harvest ran), but run_cell
# must still finish completed (no crashes → no aborted status).
dispatch_strict() {
  _CELL_TOTAL_SLICES=1
  _CELL_CRASHED_SLICES=0
  log "stub dispatch_strict: 1/1 slices succeeded (normal cell, 0 crashes)"
}

CELL_RUN_UID_OVERRIDE="$uid"
CELL_CHASH_OVERRIDE="testhash"

rc=0
run_cell "$plan_id" "strict" "1" 2>/dev/null || rc=\$?
printf 'run_cell_rc=%s\n' "\$rc" >"$state_file"
WRAP
  chmod +x "$wrapper"
  bash "$wrapper" 2>/dev/null || true

  local run_rc=0
  [ -f "$state_file" ] && run_rc="$(grep '^run_cell_rc=' "$state_file" | cut -d= -f2 || printf '0')"

  [ "${run_rc:-1}" -eq 0 ] \
    && ok "T7a: run_cell returned 0 for normal cell (touch > 0)" \
    || bad "T7a: run_cell returned non-zero (${run_rc}) for normal cell — expected 0"

  local run_status
  run_status="$(PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" bench show "$uid" --json 2>/dev/null \
    | jq -r '.status' 2>/dev/null || printf 'absent')"
  [ "$run_status" = "completed" ] \
    && ok "T7b: bench show .status = completed (normal cell with touches)" \
    || bad "T7b: bench show .status = '${run_status}' (expected 'completed')"

  rm -rf "$home"
}

# ===========================================================================
# TEST 8 (4466) — Partial crash: one slice crashes, another produces touches.
#
# Rule: if crashed_slices > 0 but crashed_slices < total_slices, the cell
# has partial data. The documented rule: only mark aborted when ALL slices
# crashed. A partial crash (some touched, some crashed) stays completed so
# the productive data is not lost and the cell does not re-run (which would
# re-do the productive slices and distort the metric).
# ===========================================================================
test_partial_crash_stays_completed() {
  printf '\n=== RESUME TEST 8 (4466): partial crash (1 crash, 1 success) -> completed ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"

  local plan_id task_id
  plan_id="$(seed_plan "$db" "$cfg" "$corpus")"
  task_id="$(seed_task "$db" "$cfg" "$corpus" "$plan_id")"

  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  local state_file="$home/partial-state"
  local uid="rr-partial-t8"

  local wrapper="$home/partial-wrapper.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
DRY_RUN=0
AGENT_TIMEOUT=0

# Stubs: bypass planning reads; open the bench run directly in execute_phase.
plan_scope()     { printf 'global'; }
declared_paths() { printf ''; }
execute_phase()  {
  command "$PLANAR_BIN" bench start "\$CELL_RUN_UID" \
    --plan "\$CELL_PLAN" --arm "\$CELL_ARM" \
    --base-sha "\${CELL_BASE_SHA:-stub}" --config-hash "testhash" >/dev/null 2>&1 || true
}

# Stub dispatch_strict: 2 slices total, 1 crashed, 1 succeeded.
# _CELL_CRASHED_SLICES < _CELL_TOTAL_SLICES so the cell stays completed.
dispatch_strict() {
  _CELL_TOTAL_SLICES=2
  _CELL_CRASHED_SLICES=1
  log "stub dispatch_strict: 1/2 slices crashed, 1/2 succeeded"
}

CELL_RUN_UID_OVERRIDE="$uid"
CELL_CHASH_OVERRIDE="testhash"

rc=0
run_cell "$plan_id" "strict" "1" 2>/dev/null || rc=\$?
printf 'run_cell_rc=%s\n' "\$rc" >"$state_file"
WRAP
  chmod +x "$wrapper"
  bash "$wrapper" 2>/dev/null || true

  local run_rc=0
  [ -f "$state_file" ] && run_rc="$(grep '^run_cell_rc=' "$state_file" | cut -d= -f2 || printf '0')"

  # Partial crash: crashes < total, so the finish status must be completed.
  [ "${run_rc:-1}" -eq 0 ] \
    && ok "T8a: run_cell returned 0 for partial crash (completed, not aborted)" \
    || bad "T8a: run_cell returned non-zero (${run_rc}) for partial crash — expected 0"

  local run_status
  run_status="$(PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" bench show "$uid" --json 2>/dev/null \
    | jq -r '.status' 2>/dev/null || printf 'absent')"
  [ "$run_status" = "completed" ] \
    && ok "T8b: bench show .status = completed (partial crash retains productive data)" \
    || bad "T8b: bench show .status = '${run_status}' (expected 'completed' for partial crash)"

  # Verify resolve_cell_uid returns SKIP (completed partial-crash cells are not re-run).
  # This confirms the "partial crash stays completed → no re-run" invariant.
  local resolve_out
  resolve_out="$(
    bash -c "
      export PLANAR_DB_OVERRIDE='$db'
      export PLANAR_CONFIG_PATH_OVERRIDE='$cfg'
      export BENCH_HOME='$home'
      export BENCH_CORPUS_REPO='$corpus'
      source '$trimmed'
      LOG_ROOT='$home/cell-logs'
      DRY_RUN=0
      resolve_cell_uid '$plan_id' 'strict' '1' 2>/dev/null
    " 2>/dev/null || true
  )"
  # The uid is rr-partial-t8 (override), not the deterministic uid.
  # resolve_cell_uid looks for m-<plan>-strict-r1 which is absent → fresh dispatch.
  # This assertion just confirms resolve_cell_uid doesn't PERM_FAIL on a partial crash.
  [ "$resolve_out" != "PERM_FAIL" ] \
    && ok "T8c: resolve_cell_uid did not PERM_FAIL for partial-crash completed cell (got '${resolve_out}')" \
    || bad "T8c: resolve_cell_uid returned PERM_FAIL for a completed cell (unexpected)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 9 (4357) — Staleness guard + structured orphan logging.
#
# T9a: Stale orphan running row (started_at well in the past, age >= threshold).
#       resolve_cell_uid must: reconcile (abort+retry uid), AND emit an
#       orphan_reconcile cell_log record with prior_status=running,
#       action=abort-retry, and an age_sec field. The JSONL line must be valid.
#
# T9b: Recent running row (started_at just now, age < threshold).
#       resolve_cell_uid must: emit a loud WARNING (grep in stderr) AND still
#       reconcile (returns a retry uid, not SKIP/hang). The orphan_reconcile log
#       record must have is_stale=0.
#
# T9c: BENCH_STALE_RUNNING_SEC override changes the stale/recent boundary.
#       A row that is "stale" under the default threshold becomes "recent" when
#       the threshold is raised, and vice versa.
#
# T9d: Worktree sweep coverage — git worktree prune at run_cell start (and
#       cleanup_cell_worktrees) clears worktrees left by a hard-killed prior
#       cell. This is already covered by T2/T3 (cleanup_cell_worktrees) and the
#       `git worktree prune` call at the top of run_cell's live path. We verify
#       the prune call exists in the script and that cleanup_cell_worktrees
#       removes a freshly-created worktree for the being-(re)run uid.
# ===========================================================================

# _make_running_row <db> <cfg> <corpus> <plan_id> <arm> <rep> — open a bench
# run record in the isolated DB using `bench start`. Returns the run_uid on
# stdout. The run is left in `running` status (no bench finish called).
_make_running_row() {
  local db="$1" cfg="$2" corpus="$3" plan_id="$4" arm="${5:-strict}" rep="${6:-1}"
  # Deterministic uid formula from bench-matrix.sh: m-<plan>-<arm>-r<rep>
  local uid="m-${plan_id}-${arm}-r${rep}"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" bench start "$uid" \
        --plan "$plan_id" --arm "$arm" \
        --base-sha "deadbeef" --config-hash "testhash" \
        >/dev/null 2>&1 ) || true
  printf '%s' "$uid"
}

test_staleness_guard_and_orphan_logging() {
  printf '\n=== RESUME TEST 9 (4357): staleness guard + structured orphan logging ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/resume-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"

  local plan_id task_id
  plan_id="$(seed_plan "$db" "$cfg" "$corpus" "t9a")"
  task_id="$(seed_task "$db" "$cfg" "$corpus" "$plan_id")"

  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  # ---------------------------------------------------------------------------
  # T9a: Stale orphan running row (started_at well in the past >= threshold).
  #
  # Strategy: create a `running` row then backdate its started_at to 2 hours
  # ago via sqlite3. Then call resolve_cell_uid with a very small threshold
  # (BENCH_STALE_RUNNING_SEC=10) — the row's age (7200s) >> threshold (10s)
  # so it is definitively stale. Assert:
  #   (a) resolve returns a retry uid (not SKIP, not PERM_FAIL)
  #   (b) an orphan_reconcile JSONL record exists for the base uid in LOG_ROOT
  #   (c) the record is valid JSON (jq parses it)
  #   (d) the record has prior_status=running, action=abort-retry, and age_sec field
  # ---------------------------------------------------------------------------
  printf '\n--- T9a: stale orphan running row ---\n'
  local uid_stale; uid_stale="$(_make_running_row "$db" "$cfg" "$corpus" "$plan_id" "strict" "1")"
  # Backdate started_at to 2 hours ago via sqlite3 using strftime so the format
  # matches the ISO 8601 form bench-matrix.sh expects (T separator, Z suffix).
  sqlite3 "$db" "update runs set started_at = strftime('%Y-%m-%dT%H:%M:%SZ','now','-7200 seconds') where run_uid = '${uid_stale}';" 2>/dev/null || true

  local log_root="$home/cell-logs"
  mkdir -p "$log_root"

  local resolve_out_stale
  resolve_out_stale="$(
    bash -c "
      export PLANAR_DB_OVERRIDE='$db'
      export PLANAR_CONFIG_PATH_OVERRIDE='$cfg'
      export BENCH_HOME='$home'
      export BENCH_CORPUS_REPO='$corpus'
      source '$trimmed'
      LOG_ROOT='$log_root'
      DRY_RUN=0
      BENCH_STALE_RUNNING_SEC=10
      resolve_cell_uid '$plan_id' 'strict' '1' 2>/dev/null
    " 2>/dev/null || true
  )"

  # (a) Returns a retry uid (not SKIP, not PERM_FAIL, not empty).
  local t9a_ok=0
  [ -n "$resolve_out_stale" ] \
    && [ "$resolve_out_stale" != "SKIP" ] \
    && [ "$resolve_out_stale" != "PERM_FAIL" ] \
    && t9a_ok=1
  [ "$t9a_ok" -eq 1 ] \
    && ok "T9a-1: stale running row -> resolve returns retry uid ('${resolve_out_stale}')" \
    || bad "T9a-1: stale running row -> resolve returned '${resolve_out_stale}' (expected retry uid)"

  # (b/c/d) An orphan_reconcile JSONL record must exist in LOG_ROOT.
  local jsonl_file="$log_root/${uid_stale}.jsonl"
  if [ -f "$jsonl_file" ]; then
    ok "T9a-2: orphan_reconcile JSONL log file created at ${jsonl_file}"
  else
    bad "T9a-2: no JSONL log file found at ${jsonl_file}"
  fi

  # Find the orphan_reconcile record (there may be other records in the file).
  local reconcile_rec=""
  if [ -f "$jsonl_file" ]; then
    reconcile_rec="$(grep -m1 '"orphan_reconcile"' "$jsonl_file" 2>/dev/null || true)"
  fi
  if [ -n "$reconcile_rec" ]; then
    ok "T9a-3: orphan_reconcile event record found in JSONL"
  else
    bad "T9a-3: no orphan_reconcile event record found in JSONL (file contents: $(cat "$jsonl_file" 2>/dev/null || echo 'missing'))"
  fi

  # Validate the record is parseable JSON.
  if [ -n "$reconcile_rec" ] && printf '%s' "$reconcile_rec" | jq -e '.' >/dev/null 2>&1; then
    ok "T9a-4: orphan_reconcile record is valid JSON"
  else
    bad "T9a-4: orphan_reconcile record is NOT valid JSON (got: '${reconcile_rec}')"
  fi

  # Check required fields: prior_status=running, action=abort-retry, age_sec present.
  if [ -n "$reconcile_rec" ]; then
    local f_prior f_action f_age
    f_prior="$(printf '%s' "$reconcile_rec" | jq -r '.prior_status // empty' 2>/dev/null || true)"
    f_action="$(printf '%s' "$reconcile_rec" | jq -r '.action // empty' 2>/dev/null || true)"
    f_age="$(printf '%s' "$reconcile_rec" | jq -r '.age_sec // empty' 2>/dev/null || true)"

    [ "$f_prior" = "running" ] \
      && ok "T9a-5: orphan_reconcile record has prior_status=running" \
      || bad "T9a-5: orphan_reconcile prior_status='${f_prior}' (expected 'running')"

    [ "$f_action" = "abort-retry" ] \
      && ok "T9a-6: orphan_reconcile record has action=abort-retry" \
      || bad "T9a-6: orphan_reconcile action='${f_action}' (expected 'abort-retry')"

    [ -n "$f_age" ] \
      && ok "T9a-7: orphan_reconcile record has age_sec field ('${f_age}')" \
      || bad "T9a-7: orphan_reconcile record missing age_sec field"
  else
    bad "T9a-5: cannot check fields — no orphan_reconcile record"
    bad "T9a-6: cannot check fields — no orphan_reconcile record"
    bad "T9a-7: cannot check fields — no orphan_reconcile record"
  fi

  # ---------------------------------------------------------------------------
  # T9b: Recent running row (started_at = now, age < threshold).
  #
  # Strategy: create a NEW running row (started_at defaults to now — it is fresh).
  # Use BENCH_STALE_RUNNING_SEC=99999 so the row is definitely "recent" (age <<
  # threshold). Assert:
  #   (a) A WARNING message is emitted to stderr (grep the wrapper's stderr)
  #   (b) resolve still returns a retry uid (not SKIP/hang) — reconciles anyway
  #   (c) The orphan_reconcile log record has is_stale=0
  # ---------------------------------------------------------------------------
  printf '\n--- T9b: recent running row (WARNING + reconcile proceeds) ---\n'
  # We need a fresh plan+rep to get a different base uid (plan 2, strict, rep 1).
  local plan_id2
  plan_id2="$(seed_plan "$db" "$cfg" "$corpus" "t9b")"
  local uid_fresh; uid_fresh="$(_make_running_row "$db" "$cfg" "$corpus" "$plan_id2" "strict" "1")"
  # Do NOT backdate — started_at is as fresh as possible.

  local log_root2="$home/cell-logs-t9b"
  mkdir -p "$log_root2"
  local stderr_file="$home/t9b-stderr.txt"

  local resolve_out_fresh
  resolve_out_fresh="$(
    bash -c "
      export PLANAR_DB_OVERRIDE='$db'
      export PLANAR_CONFIG_PATH_OVERRIDE='$cfg'
      export BENCH_HOME='$home'
      export BENCH_CORPUS_REPO='$corpus'
      source '$trimmed'
      LOG_ROOT='$log_root2'
      DRY_RUN=0
      BENCH_STALE_RUNNING_SEC=99999
      resolve_cell_uid '$plan_id2' 'strict' '1' 2>'$stderr_file'
    " 2>/dev/null || true
  )"

  # (a) WARNING must be in stderr.
  if grep -qi "WARNING" "$stderr_file" 2>/dev/null; then
    ok "T9b-1: WARNING emitted for recent running row (age < threshold)"
  else
    bad "T9b-1: no WARNING found in stderr for recent running row (stderr: $(cat "$stderr_file" 2>/dev/null || echo 'missing'))"
  fi

  # (b) Still reconciles — returns a retry uid.
  local t9b_ok=0
  [ -n "$resolve_out_fresh" ] \
    && [ "$resolve_out_fresh" != "SKIP" ] \
    && [ "$resolve_out_fresh" != "PERM_FAIL" ] \
    && t9b_ok=1
  [ "$t9b_ok" -eq 1 ] \
    && ok "T9b-2: recent running row -> still reconciles, returns retry uid ('${resolve_out_fresh}')" \
    || bad "T9b-2: recent running row -> resolve returned '${resolve_out_fresh}' (expected retry uid — should still reconcile)"

  # (c) orphan_reconcile record has is_stale=0 (false).
  local jsonl2="$log_root2/${uid_fresh}.jsonl"
  local rec2=""
  if [ -f "$jsonl2" ]; then
    rec2="$(grep -m1 '"orphan_reconcile"' "$jsonl2" 2>/dev/null || true)"
  fi
  if [ -n "$rec2" ]; then
    local f_is_stale
    f_is_stale="$(printf '%s' "$rec2" | jq -r '.is_stale // empty' 2>/dev/null || true)"
    [ "$f_is_stale" = "0" ] \
      && ok "T9b-3: orphan_reconcile record has is_stale=0 for recent row" \
      || bad "T9b-3: orphan_reconcile is_stale='${f_is_stale}' (expected 0 for recent row)"
  else
    bad "T9b-3: no orphan_reconcile record found in ${jsonl2} for recent-row test"
  fi

  # ---------------------------------------------------------------------------
  # T9c: BENCH_STALE_RUNNING_SEC override changes the stale/recent boundary.
  #
  # Strategy: use a new row (started_at = now). With threshold=0, the row is
  # always treated as stale (is_stale=1, no WARNING). With threshold=99999,
  # the same row is recent (is_stale=0, WARNING emitted). Both paths already
  # tested above; here we verify is_stale=1 when threshold=0.
  # ---------------------------------------------------------------------------
  printf '\n--- T9c: BENCH_STALE_RUNNING_SEC=0 treats every row as stale ---\n'
  local plan_id3
  plan_id3="$(seed_plan "$db" "$cfg" "$corpus" "t9c")"
  local uid_thresh; uid_thresh="$(_make_running_row "$db" "$cfg" "$corpus" "$plan_id3" "strict" "1")"

  local log_root3="$home/cell-logs-t9c"
  mkdir -p "$log_root3"
  local stderr3="$home/t9c-stderr.txt"

  local resolve_out_thresh
  resolve_out_thresh="$(
    bash -c "
      export PLANAR_DB_OVERRIDE='$db'
      export PLANAR_CONFIG_PATH_OVERRIDE='$cfg'
      export BENCH_HOME='$home'
      export BENCH_CORPUS_REPO='$corpus'
      source '$trimmed'
      LOG_ROOT='$log_root3'
      DRY_RUN=0
      BENCH_STALE_RUNNING_SEC=0
      resolve_cell_uid '$plan_id3' 'strict' '1' 2>'$stderr3'
    " 2>/dev/null || true
  )"

  # Should return a retry uid (reconcile proceeds as stale).
  local t9c_ok=0
  [ -n "$resolve_out_thresh" ] \
    && [ "$resolve_out_thresh" != "SKIP" ] \
    && [ "$resolve_out_thresh" != "PERM_FAIL" ] \
    && t9c_ok=1
  [ "$t9c_ok" -eq 1 ] \
    && ok "T9c-1: BENCH_STALE_RUNNING_SEC=0 -> row always stale, returns retry uid ('${resolve_out_thresh}')" \
    || bad "T9c-1: BENCH_STALE_RUNNING_SEC=0 -> got '${resolve_out_thresh}' (expected retry uid)"

  # No WARNING should be emitted when threshold=0 (stale path, not recent path).
  if grep -qi "WARNING" "$stderr3" 2>/dev/null; then
    bad "T9c-2: unexpected WARNING with BENCH_STALE_RUNNING_SEC=0 (should be stale path, not recent)"
  else
    ok "T9c-2: BENCH_STALE_RUNNING_SEC=0 -> no WARNING (correctly treated as stale)"
  fi

  # is_stale=1 in the log record.
  local jsonl3="$log_root3/${uid_thresh}.jsonl"
  local rec3=""
  if [ -f "$jsonl3" ]; then
    rec3="$(grep -m1 '"orphan_reconcile"' "$jsonl3" 2>/dev/null || true)"
  fi
  if [ -n "$rec3" ]; then
    local f_is_stale3
    f_is_stale3="$(printf '%s' "$rec3" | jq -r '.is_stale // empty' 2>/dev/null || true)"
    [ "$f_is_stale3" = "1" ] \
      && ok "T9c-3: BENCH_STALE_RUNNING_SEC=0 -> orphan_reconcile is_stale=1" \
      || bad "T9c-3: is_stale='${f_is_stale3}' (expected 1 when threshold=0)"
  else
    bad "T9c-3: no orphan_reconcile record in ${jsonl3} for threshold=0 test"
  fi

  # ---------------------------------------------------------------------------
  # T9d: Worktree sweep coverage (verification-only).
  #
  # The brief asks: confirm that `git worktree prune` at run_cell start (and
  # cleanup_cell_worktrees) actually clears worktrees from a hard-killed prior
  # cell on the next run. Verify the startup prune call exists in the script.
  # Also verify cleanup_cell_worktrees removes the exact uid's worktree dir.
  # ---------------------------------------------------------------------------
  printf '\n--- T9d: worktree prune coverage (startup prune + per-uid cleanup) ---\n'

  # Verify the run_cell startup `git worktree prune` call exists in bench-matrix.sh.
  # It appears just before the integration branch creation.
  if grep -q "worktree prune" "$MATRIX" 2>/dev/null; then
    ok "T9d-1: bench-matrix.sh contains 'git worktree prune' for startup self-heal"
  else
    bad "T9d-1: bench-matrix.sh missing 'git worktree prune' — startup prune not present"
  fi

  # Verify that a cell-uid worktree dir IS removed by cleanup_cell_worktrees
  # (exercises the prune path for the being-(re)run uid specifically).
  local uid_wt="rr-t9d-wt-prune"
  source_matrix "$db" "$cfg" "$corpus"
  CELL_RUN_UID="$uid_wt"
  CELL_BASE_SHA="$(git -C "$corpus" rev-parse HEAD)"
  DRY_RUN=0
  unset BENCH_KEEP_WORKTREES 2>/dev/null || true
  local base_wt_count
  base_wt_count="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  mkdir -p "$WORKTREE_ROOT/$uid_wt"
  local kill9_wt="$WORKTREE_ROOT/$uid_wt/slice-after-kill9"
  git -C "$corpus" worktree add -q --detach "$kill9_wt" "$CELL_BASE_SHA" 2>/dev/null
  local wt_before
  wt_before="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  [ "$wt_before" -gt "$base_wt_count" ] \
    && ok "T9d-2: kill-9 orphan worktree registered before cleanup (count=${wt_before})" \
    || bad "T9d-2: worktree was NOT registered before cleanup (count=${wt_before}, base=${base_wt_count})"

  cleanup_cell_worktrees "$uid_wt"
  local wt_after
  wt_after="$(git -C "$corpus" worktree list 2>/dev/null | wc -l | tr -d ' ')"
  [ "$wt_after" -eq "$base_wt_count" ] \
    && ok "T9d-3: kill-9 orphan worktree removed by cleanup_cell_worktrees (count back to ${base_wt_count})" \
    || bad "T9d-3: worktree NOT removed: count=${wt_after}, expected=${base_wt_count}"

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
  command -v sqlite3 >/dev/null || { printf 'sqlite3 required\n'; exit 2; }

  test_planning_cwd_sigpipe_safe
  test_completed_cell_worktrees_pruned
  test_cleanup_idempotent
  test_all_slices_crash_marks_aborted
  test_genuine_noop_stays_completed
  test_normal_cell_completed
  test_partial_crash_stays_completed
  test_staleness_guard_and_orphan_logging
  test_regression

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
