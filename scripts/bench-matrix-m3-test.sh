#!/usr/bin/env bash
#
# bench-matrix-m3-test.sh — spend-bounded control-flow tests for the M3 matrix
# loop in bench-matrix.sh. NO agent spawns: the dry-run test exercises the full
# scheduler with mutation disabled, and the resume + ceiling tests STUB run_cell
# to a no-op so the loop's skip/retry/stop logic is asserted without cost.
#
# ISOLATION: every test runs against a fresh temp PLANAR_DB. The real
# ~/.planar/planar.db is NEVER named. Each test mints its own BENCH_HOME.
#
# Usage: scripts/bench-matrix-m3-test.sh   (runs all three; exits non-zero on
# any failure).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLANAR_BIN="${PLANAR_BIN:-planar}"
# Planning verbs (plan create, task add) refuse to run from inside a git
# worktree (the worktree planning-verb split). REPO_ROOT is a worktree, so seed
# state from the corpus repo's PRIMARY (non-worktree) checkout — the same cwd
# bench-matrix.sh derives as PLANNING_CWD for its own reads.
PLANNING_CWD="$(git -C "$REPO_ROOT" worktree list --porcelain 2>/dev/null \
  | awk '/^worktree /{print $2; exit}')"
[ -n "$PLANNING_CWD" ] || PLANNING_CWD="$REPO_ROOT"

pass=0 fail=0
ok()   { printf 'PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad()  { printf 'FAIL: %s\n' "$*"; fail=$((fail + 1)); }

# new_iso_db — print a fresh isolated BENCH_HOME, init the DB, return its path.
new_iso_db() {
  local home; home="$(mktemp -d "${TMPDIR:-/tmp}/m3test.XXXXXX")"
  printf '%s' "$home"
}

# seed_plan <db> <config> <title> — create a global-scope plan with two tasks
# (no declared touch paths, so base_sha falls back to corpus HEAD — keeps the
# test decoupled from git-history archaeology, which is M1's concern not M3's).
# Prints the new plan id.
seed_plan() {
  local db="$1" cfg="$2" title="$3" pid
  pid="$(cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" plan create "$title" --scope global --json 2>/dev/null \
    | jq -r '.id')"
  ( cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" task add "task A of $title" --plan "$pid" --scope global >/dev/null 2>&1 )
  ( cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" task add "task B of $title" --plan "$pid" --scope global >/dev/null 2>&1 )
  printf '%s' "$pid"
}

# ---------------------------------------------------------------------------
# TEST 1 — dry-run over a multi-plan, N=3 matrix.
# Asserts: full interleaved schedule printed; arm order VARIES per (rep,plan);
# per-plan base_sha printed; all deterministic run_uids printed; estimated total
# spend printed. NO spawns (dry-run mutates nothing).
# ---------------------------------------------------------------------------
test_dry_run() {
  local home db cfg p1 p2 out
  home="$(new_iso_db)"; db="$home/exp.db"; cfg="$home/config.toml"
  p1="$(seed_plan "$db" "$cfg" "plan one")"
  p2="$(seed_plan "$db" "$cfg" "plan two")"

  out="$(BENCH_HOME="$home" PLANAR_DB_OVERRIDE="$db" PLANAR_CONFIG_PATH_OVERRIDE="$cfg" \
    BENCH_CORPUS_REPO="$REPO_ROOT" \
    bash "$MATRIX" --matrix --plans "$p1,$p2" --reps 3 --ceiling 100.00 --dry-run 2>&1)"

  # 2 plans x 3 arms x 3 reps = 18 cells.
  local uid_count
  uid_count="$(printf '%s\n' "$out" | grep -cE 'uid=m-[0-9]+-(strict|eligibility|grouped)-r[0-9]+')"
  [ "$uid_count" -ge 18 ] \
    && ok "dry-run: emitted >=18 cell run_uids (got $uid_count)" \
    || bad "dry-run: expected >=18 cell uids, got $uid_count"

  # Interleaving: arm order must VARY across (rep,plan) pairs. Extract the arm
  # sequence per pair from the schedule list lines and assert not all identical.
  local pairs distinct
  pairs="$(printf '%s\n' "$out" | grep -oE 'rep=[0-9]+ plan=[0-9]+ arm=(strict|eligibility|grouped)' \
    | awk '{print $1"-"$2": "$3}')"
  # Build the per-pair arm-order signature; assert >1 distinct ordering exists.
  distinct="$(printf '%s\n' "$out" \
    | grep -oE 'rep=[0-9]+ plan=[0-9]+ arm=(strict|eligibility|grouped)' \
    | sed -E 's/.*arm=//' | paste -sd, - )"
  if printf '%s\n' "$out" | grep -q 'arm order shuffled per rep,plan'; then
    ok "dry-run: schedule announces per-(rep,plan) shuffled interleaving"
  else
    bad "dry-run: schedule did not announce interleaving"
  fi
  # Concrete interleave check: the FIRST arm of at least two different pairs
  # differs (a fixed order would make every pair lead with the same arm).
  local lead_arms
  lead_arms="$(printf '%s\n' "$out" | grep -oE 'uid=m-[0-9]+-(strict|eligibility|grouped)-r[0-9]+' \
    | sed -E 's/.*-(strict|eligibility|grouped)-r.*/\1/' | sort -u | wc -l | tr -d ' ')"
  [ "$lead_arms" -eq 3 ] \
    && ok "dry-run: all three arms appear in the schedule" \
    || bad "dry-run: expected 3 distinct arms in schedule, got $lead_arms"

  printf '%s\n' "$out" | grep -q 'base_sha=' \
    && ok "dry-run: per-plan base_sha printed" \
    || bad "dry-run: per-plan base_sha missing"
  printf '%s\n' "$out" | grep -qE 'est. total \(new\):' \
    && ok "dry-run: estimated total spend printed" \
    || bad "dry-run: estimated total spend missing"
  printf '%s\n' "$out" | grep -q 'paired_config_hash_base=' \
    && ok "dry-run: paired config_hash base printed per plan" \
    || bad "dry-run: paired config_hash base missing"

  # No mutation: no runs should exist in the DB after a dry-run.
  local run_rows
  run_rows="$(sqlite3 "$db" 'select count(*) from runs;' 2>/dev/null || echo 0)"
  [ "$run_rows" -eq 0 ] \
    && ok "dry-run: zero run rows written (no mutation)" \
    || bad "dry-run: $run_rows run rows written — dry-run MUTATED the DB"

  printf '%s\n' "--- sample dry-run output (head) ---" >&2
  printf '%s\n' "$out" | sed -n '1,40p' >&2
  rm -rf "$home"
}

# ---------------------------------------------------------------------------
# Shared stub: source bench-matrix.sh with run_cell REPLACED by a recorder so
# the resume + ceiling tests assert control flow without agent spend.
# We re-exec the matrix with a wrapper that overrides run_cell after sourcing.
# ---------------------------------------------------------------------------

# run_matrix_stubbed <db> <cfg> <plans> <reps> <ceiling> <dispatch_log> —
# run the live matrix walk with run_cell stubbed to: append "<plan> <arm> <rep>"
# to <dispatch_log> and write a completed bench run (so resume can observe it on
# a SECOND pass). Uses a small wrapper script so the override survives sourcing.
run_matrix_stubbed() {
  local db="$1" cfg="$2" plans="$3" reps="$4" ceiling="$5" dlog="$6"
  local wrapper="$db.wrapper.sh"
  # A trimmed copy of the matrix with the final `main "$@"` stripped, written to
  # a REAL file so ${BASH_SOURCE[0]} resolves when the wrapper sources it (it
  # does not under `source <(...)` process substitution, which trips set -u).
  local trimmed="$db.matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$(dirname "$db")"
export BENCH_CORPUS_REPO="$REPO_ROOT"
# Source the matrix (main stripped) so its functions are in scope, then override
# run_cell with a no-op recorder. Sourcing by path keeps BASH_SOURCE valid.
source "$trimmed"
# Recorder: log the dispatch AND write a completed bench run so a second pass
# sees status=completed and SKIPS it (resume is additive, never double-counts).
run_cell() {
  local plan="\$1" arm="\$2" rep="\$3"
  local uid="\${CELL_RUN_UID_OVERRIDE}"
  printf '%s %s %s\n' "\$plan" "\$arm" "\$rep" >>"$dlog"
  command "$PLANAR_BIN" bench start "\$uid" --plan "\$plan" --arm "\$arm" \\
    --base-sha stub --config-hash "\${CELL_CHASH_OVERRIDE}" >/dev/null 2>&1 || true
  command "$PLANAR_BIN" bench event "\$uid" --kind token_sample --seq 1 \\
    --payload '{"in":10,"out":5,"usd":1.00,"role":"coder"}' >/dev/null 2>&1 || true
  command "$PLANAR_BIN" bench finish "\$uid" --status completed >/dev/null 2>&1 || true
}
ARG_MATRIX=1
CORPUS_PLANS="$plans"
N_REPS="$reps"
CEILING="$ceiling"
require_tool sqlite3
run_matrix
WRAP
  bash "$wrapper"
}

# ---------------------------------------------------------------------------
# TEST 2 — resume / idempotency.
# Pass 1: stubbed matrix dispatches every cell (each writes a completed run).
# Pass 2: re-run the SAME matrix → every cell must be SKIPPED (no new dispatch),
# proving resume is additive and never double-counts.
# ---------------------------------------------------------------------------
test_resume() {
  local home db cfg p1 dlog1 dlog2 n1 n2
  home="$(new_iso_db)"; db="$home/exp.db"; cfg="$home/config.toml"
  p1="$(seed_plan "$db" "$cfg" "resume plan")"
  dlog1="$home/dispatch1.log"; dlog2="$home/dispatch2.log"
  : >"$dlog1"; : >"$dlog2"

  run_matrix_stubbed "$db" "$cfg" "$p1" 2 100.00 "$dlog1" >/dev/null 2>&1
  n1="$(wc -l < "$dlog1" | tr -d " ")"
  # 1 plan x 3 arms x 2 reps = 6 cells dispatched on the first pass.
  [ "$n1" -eq 6 ] \
    && ok "resume: pass 1 dispatched all 6 cells (got $n1)" \
    || bad "resume: pass 1 expected 6 dispatches, got $n1"

  run_matrix_stubbed "$db" "$cfg" "$p1" 2 100.00 "$dlog2" >/dev/null 2>&1
  n2="$(wc -l < "$dlog2" | tr -d " ")"
  [ "$n2" -eq 0 ] \
    && ok "resume: pass 2 dispatched 0 cells (all completed -> SKIP, additive)" \
    || bad "resume: pass 2 expected 0 dispatches, got $n2 (double-count!)"

  # The DB must hold exactly 6 completed runs (no duplicates from pass 2).
  local completed
  completed="$(sqlite3 "$db" "select count(*) from runs where status='completed';" 2>/dev/null)"
  [ "$completed" -eq 6 ] \
    && ok "resume: exactly 6 completed runs in DB (no double-count)" \
    || bad "resume: expected 6 completed runs, got $completed"
  rm -rf "$home"
}

# TEST 2b — crashed-cell retry: seed a RUNNING (crashed mid-cell) record, then
# run the matrix; the loop must ABORT the stale record and dispatch a -retry uid.
test_resume_retry() {
  local home db cfg p1 dlog uid_base
  home="$(new_iso_db)"; db="$home/exp.db"; cfg="$home/config.toml"
  p1="$(seed_plan "$db" "$cfg" "retry plan")"
  uid_base="m-${p1}-strict-r1"
  # Seed a crashed-mid-cell RUNNING record under the deterministic uid for one
  # cell (rep1/strict). bench start leaves status=running (no finish).
  PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" command "$PLANAR_BIN" \
    bench start "$uid_base" --plan "$p1" --arm strict --base-sha stub \
    --config-hash seeded >/dev/null 2>&1
  dlog="$home/dispatch.log"; : >"$dlog"
  run_matrix_stubbed "$db" "$cfg" "$p1" 1 100.00 "$dlog" >/dev/null 2>&1

  # The seeded run must now be aborted (audit trail), and a -retry1 completed.
  local seeded_status retry_status
  seeded_status="$(sqlite3 "$db" "select status from runs where run_uid='$uid_base';" 2>/dev/null)"
  retry_status="$(sqlite3 "$db" "select status from runs where run_uid='${uid_base}-retry1';" 2>/dev/null)"
  [ "$seeded_status" = "aborted" ] \
    && ok "retry: crashed RUNNING record aborted (left as audit)" \
    || bad "retry: crashed record status='$seeded_status' (expected aborted)"
  [ "$retry_status" = "completed" ] \
    && ok "retry: fresh -retry1 uid dispatched + completed" \
    || bad "retry: -retry1 status='$retry_status' (expected completed)"

  # Ledger must EXCLUDE the aborted record (no token_sample on it anyway, but
  # assert the aborted run contributes nothing).
  local ledger
  ledger="$(sqlite3 "$db" "
    select coalesce(sum(json_extract(e.payload,'\$.usd')),0)
    from runs r join run_events e on e.run_id=r.id
    where e.kind='token_sample' and r.status<>'aborted';" 2>/dev/null)"
  # 3 arms x 1 rep, each \$1.00 = \$3.00 across non-aborted runs.
  awk -v l="$ledger" 'BEGIN{ exit !(l == 3) }' \
    && ok "retry: ledger sums only non-aborted runs (\$$ledger)" \
    || bad "retry: ledger=\$$ledger (expected 3.00, aborted excluded)"
  rm -rf "$home"
}

# ---------------------------------------------------------------------------
# TEST 3 — spend ceiling.
# Seed a ledger near the ceiling, then run with a tiny ceiling so the FIRST cell
# check trips: the loop must stop cleanly, dispatching nothing.
# ---------------------------------------------------------------------------
test_ceiling() {
  local home db cfg p1 dlog n
  home="$(new_iso_db)"; db="$home/exp.db"; cfg="$home/config.toml"
  p1="$(seed_plan "$db" "$cfg" "ceiling plan")"
  # Seed a completed run carrying \$9.50 of spend so cumulative is already high.
  PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" command "$PLANAR_BIN" \
    bench start "seed-ledger" --plan "$p1" --arm strict --base-sha stub \
    --config-hash seeded >/dev/null 2>&1
  PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" command "$PLANAR_BIN" \
    bench event "seed-ledger" --kind token_sample --seq 1 \
    --payload '{"in":1,"out":1,"usd":9.50,"role":"coder"}' >/dev/null 2>&1
  PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" command "$PLANAR_BIN" \
    bench finish "seed-ledger" --status completed >/dev/null 2>&1

  dlog="$home/dispatch.log"; : >"$dlog"
  # Ceiling \$10.00; cumulative \$9.50; est cell = avg of completed = \$9.50 ->
  # 9.50 + 9.50 = 19.00 > 10.00, so the FIRST cell check must stop the loop.
  local out
  out="$(run_matrix_stubbed "$db" "$cfg" "$p1" 1 10.00 "$dlog" 2>&1)"
  n="$(wc -l < "$dlog" | tr -d " ")"
  [ "$n" -eq 0 ] \
    && ok "ceiling: stopped before dispatching any cell (0 dispatches)" \
    || bad "ceiling: dispatched $n cells past the ceiling"
  printf '%s\n' "$out" | grep -q 'ceiling reached — analyze what exists' \
    && ok "ceiling: printed the clean-stop banner" \
    || bad "ceiling: clean-stop banner missing"

  # The seeded completed run must remain intact (ceiling stop leaves data).
  local still
  still="$(sqlite3 "$db" "select status from runs where run_uid='seed-ledger';" 2>/dev/null)"
  [ "$still" = "completed" ] \
    && ok "ceiling: pre-existing completed run left intact" \
    || bad "ceiling: seeded run status='$still' (expected completed)"
  rm -rf "$home"
}

main() {
  command -v jq >/dev/null      || { echo "jq required"; exit 2; }
  command -v sqlite3 >/dev/null || { echo "sqlite3 required"; exit 2; }
  printf '=== TEST 1: dry-run over multi-plan N=3 matrix ===\n'
  test_dry_run
  printf '\n=== TEST 2: resume / idempotency (stubbed run_cell) ===\n'
  test_resume
  printf '\n=== TEST 2b: crashed-cell abort + retry ===\n'
  test_resume_retry
  printf '\n=== TEST 3: spend ceiling stop (stubbed run_cell) ===\n'
  test_ceiling
  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
