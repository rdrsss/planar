#!/usr/bin/env bash
#
# bench-matrix-m4-test.sh — tests for the M4 hardening in bench-matrix.sh:
#   1. Per-agent/per-cell timeout + kill (pure-bash watchdog, pure-bash path)
#   2. Trap-based cleanup on SIGINT
#   3. Per-cell retry cap
#   4. Structured per-cell JSONL logging
#   5. No regression against M3 and B1 tests
#
# NO real claude invocations. Fake claude stubs are tiny bash scripts. No
# network. No real spend. Exits non-zero when any assertion fails.
#
# Convention (matches bench-matrix-m3-test.sh):
#   - All matrix-sourcing logic runs inside wrapper scripts (NOT sourced into
#     this shell) so the harness trap handlers do not leak into this test process.
#   - Each test function creates its own BENCH_HOME and cleans up on success.
#
# Usage: scripts/bench-matrix-m4-test.sh
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
  mktemp -d "${TMPDIR:-/tmp}/m4test.XXXXXX"
}

# seed_plan <db> <cfg> <title> — create a global-scope plan with one task.
# Prints the new plan id.
seed_plan() {
  local db="$1" cfg="$2" title="$3" pid
  pid="$(cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" plan create "$title" --scope global --json 2>/dev/null \
    | jq -r '.id')"
  ( cd "$PLANNING_CWD" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" task add "task A of $title" --plan "$pid" \
    --scope global >/dev/null 2>&1 )
  printf '%s' "$pid"
}

# trimmed_matrix <dest> — copy the matrix with the final `main "$@"` stripped,
# so sourcing it does not auto-run. Prints the dest path.
trimmed_matrix() {
  local dest="$1"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$dest"
  printf '%s' "$dest"
}

# ===========================================================================
# TEST 1 — Timeout watchdog: fires + kills entire process group (pure-bash)
# ===========================================================================
# The watchdog test runs inside a wrapper script so the harness trap handlers
# do NOT install themselves in this test's shell process. The wrapper:
#   - sources the matrix (installs traps in the wrapper's shell)
#   - sets AGENT_TIMEOUT=2 (2s deadline)
#   - calls spawn_agent_watchdog with a stub `claude` that sleeps 20s AND
#     spawns a child subprocess
#   - reports results (pids-file, rc, AGENT_TIMED_OUT flag) to a state dir
test_timeout_watchdog() {
  printf '\n=== M4 TEST 1: timeout watchdog (pure-bash path) ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  # Build a minimal corpus git repo.
  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t1@test.local
  git -C "$corpus" config user.name m4t1
  printf 'base\n' >"$corpus/seed.txt"
  git -C "$corpus" add -A
  git -C "$corpus" commit -qm base

  # Worktree for the agent call.
  local wt="$home/slice-t1"
  git -C "$corpus" worktree add -q --detach "$wt" HEAD

  # Fake slow claude stub: sleeps 20s, records own PID + one child PID.
  local stub_dir="$home/stub-dir"
  mkdir -p "$stub_dir"
  cat >"$stub_dir/claude" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$\$" >>"${home}/stub-pids"
sleep 9999 &
CHILD=\$!
printf '%s\n' "\$CHILD" >>"${home}/stub-pids"
sleep 20
kill "\$CHILD" 2>/dev/null || true
printf '%s\n' '{"usage":{"input_tokens":1,"output_tokens":1,"cache_read_input_tokens":0,"cache_creation_input_tokens":0},"total_cost_usd":0.001,"result":"done"}'
STUB
  chmod +x "$stub_dir/claude"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"
  local state_file="$home/watchdog-state"

  # Wrapper: sources matrix, calls spawn_agent_watchdog with the flag-file
  # mechanism (same pattern spawn_agent uses), writes state file.
  # DISABLES gtimeout/timeout (not on PATH on macOS) to exercise pure-bash path.
  local wrapper="$home/watchdog-wrapper.sh"
  cat >"$wrapper" <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# Add the stub dir to PATH so `claude` resolves to our fake stub.
export PATH="${stub_dir}:\${PATH}"
# source the trimmed matrix (traps install in THIS wrapper's shell, not the test's)
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
# Force the pure-bash watchdog: small timeout.
AGENT_TIMEOUT=2
DRY_RUN=0
# Use the flag-file mechanism (same as spawn_agent) so AGENT_TIMED_OUT is set
# in THIS shell, not in a subshell.
_toflag="\$(mktemp "\${TMPDIR:-/tmp}/toflag.XXXXXX")"
rm -f "\$_toflag"
_AGENT_TIMEOUT_FLAG="\$_toflag"
AGENT_TIMED_OUT=0
before=\$(date +%s)
rc=0
# Run watchdog in subshell via \$() — flag file persists across the subshell boundary.
out="\$(spawn_agent_watchdog "stub-model" "$wt" "test brief" 2>/dev/null)" || rc=\$?
after=\$(date +%s)
elapsed=\$(( after - before ))
# Check the flag file to detect timeout (the only cross-subshell channel).
[ -f "\$_toflag" ] && AGENT_TIMED_OUT=1
rm -f "\$_toflag" 2>/dev/null || true
_AGENT_TIMEOUT_FLAG=""
printf 'rc=%s\n'          "\$rc"               >"$state_file"
printf 'timed_out=%s\n'   "\${AGENT_TIMED_OUT}" >>"$state_file"
printf 'elapsed=%s\n'     "\$elapsed"           >>"$state_file"
WRAPPER
  chmod +x "$wrapper"

  local before_ts; before_ts="$(date +%s)"
  bash "$wrapper" 2>/dev/null || true
  local after_ts; after_ts="$(date +%s)"
  local wall=$(( after_ts - before_ts ))

  # Read wrapper results.
  local wrc=0 wtimed_out=0 welapsed=0
  if [ -f "$state_file" ]; then
    wrc="$(grep '^rc=' "$state_file" | cut -d= -f2)"
    wtimed_out="$(grep '^timed_out=' "$state_file" | cut -d= -f2)"
    welapsed="$(grep '^elapsed=' "$state_file" | cut -d= -f2)"
  fi

  # 1a. Watchdog must return non-zero when the deadline fires.
  [ "${wrc:-0}" -ne 0 ] \
    && ok "timeout: spawn_agent_watchdog returned non-zero on deadline breach (rc=$wrc)" \
    || bad "timeout: watchdog returned 0 — deadline did not fire (rc=$wrc)"

  # 1b. AGENT_TIMED_OUT flag must be set.
  [ "${wtimed_out:-0}" -eq 1 ] \
    && ok "timeout: AGENT_TIMED_OUT=1 reported by wrapper" \
    || bad "timeout: AGENT_TIMED_OUT=${wtimed_out:-0} (expected 1)"

  # 1c. Total wall-clock must be close to the deadline, not 20s.
  [ "$wall" -le 12 ] \
    && ok "timeout: wall-clock time ${wall}s <= 12s (deadline=2s, proves watchdog fired)" \
    || bad "timeout: wall-clock ${wall}s > 12s — watchdog too slow or did not fire"

  # 1d. Child subprocess spawned by the stub must be dead (no orphan).
  # Give processes a moment to die from the kill.
  sleep 1
  local orphans=0
  if [ -f "$home/stub-pids" ]; then
    local pid
    while IFS= read -r pid; do
      [ -n "$pid" ] || continue
      if kill -0 "$pid" 2>/dev/null; then
        orphans=$((orphans + 1))
        kill "$pid" 2>/dev/null || true
      fi
    done <"$home/stub-pids"
  fi
  [ "$orphans" -eq 0 ] \
    && ok "timeout: no orphaned stub processes after watchdog kill (pids checked)" \
    || bad "timeout: $orphans orphaned process(es) survived the watchdog kill"

  # 1e. AGENT_TIMEOUT=0 disables the watchdog; a fast stub must succeed.
  local fast_dir="$home/fast-dir"
  mkdir -p "$fast_dir"
  cat >"$fast_dir/claude" <<FAST
#!/usr/bin/env bash
printf '%s\n' '{"usage":{"input_tokens":1,"output_tokens":1,"cache_read_input_tokens":0,"cache_creation_input_tokens":0},"total_cost_usd":0.001,"result":"done"}'
FAST
  chmod +x "$fast_dir/claude"
  local fast_state="$home/fast-state"
  local fast_wrapper="$home/fast-wrapper.sh"
  cat >"$fast_wrapper" <<FWRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
export PATH="${fast_dir}:\${PATH}"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
AGENT_TIMEOUT=0
DRY_RUN=0
rc=0
out="\$(spawn_agent_watchdog "stub-model" "$wt" "brief" 2>/dev/null)" || rc=\$?
printf 'rc=%s\n' "\$rc" >"$fast_state"
FWRAP
  chmod +x "$fast_wrapper"
  bash "$fast_wrapper" 2>/dev/null || true
  local fast_rc=0
  [ -f "$fast_state" ] && fast_rc="$(grep '^rc=' "$fast_state" | cut -d= -f2)"
  [ "${fast_rc:-1}" -eq 0 ] \
    && ok "timeout: AGENT_TIMEOUT=0 disables watchdog, fast stub succeeds (rc=0)" \
    || bad "timeout: AGENT_TIMEOUT=0 path returned rc=${fast_rc:-?} (expected 0)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 2 — Trap cleanup on SIGINT: no orphaned processes, run row left resumable
# ===========================================================================
test_trap_cleanup() {
  printf '\n=== M4 TEST 2: trap cleanup on SIGINT ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"
  local p; p="$(seed_plan "$db" "$cfg" "trap test plan")"

  # Fake claude that sleeps indefinitely and records its pids.
  local stub_dir="$home/stub-trap"
  mkdir -p "$stub_dir"
  cat >"$stub_dir/claude" <<TRAPS
#!/usr/bin/env bash
printf '%s\n' "\$\$" >>"${home}/trap-pids"
sleep 9999 &
CHILD=\$!
printf '%s\n' "\$CHILD" >>"${home}/trap-pids"
wait "\$CHILD"
TRAPS
  chmod +x "$stub_dir/claude"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  # Wrapper that runs a stubbed matrix with a long-running cell.
  # The cell opens a bench run (leaving it in status=running) and then
  # invokes spawn_agent_watchdog (which will block indefinitely because
  # AGENT_TIMEOUT is large). We SIGINT the wrapper.
  local wrapper="$home/trap-wrapper.sh"
  cat >"$wrapper" <<TRAPW
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$REPO_ROOT"
export PATH="${stub_dir}:\${PATH}"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
AGENT_TIMEOUT=60

# Stub run_cell: open a bench run, then block in spawn_agent_watchdog.
run_cell() {
  local plan="\$1" arm="\$2" rep="\$3"
  local uid="\${CELL_RUN_UID_OVERRIDE}"
  CELL_RUN_UID="\$uid"; CELL_PLAN="\$plan"; CELL_ARM="\$arm"
  _INFLIGHT_CELL_UID="\$uid"
  _INFLIGHT_CELL_DIRS=()
  command "$PLANAR_BIN" bench start "\$uid" --plan "\$plan" --arm "\$arm" \\
    --base-sha stub --config-hash seeded >/dev/null 2>&1 || true
  # Block in the watchdog — this is what gets SIGINT'd.
  spawn_agent_watchdog "stub-model" "$REPO_ROOT" "brief" >/dev/null 2>/dev/null || true
}
ARG_MATRIX=1
CORPUS_PLANS="$p"
N_REPS=1
CEILING=999.00
require_tool sqlite3
run_matrix 2>&1
TRAPW
  chmod +x "$wrapper"

  bash "$wrapper" &
  local wrapper_pid=$!
  # Allow the cell to start and spawn the fake claude.
  sleep 2
  # SIGINT the wrapper.
  kill -INT "$wrapper_pid" 2>/dev/null || true
  wait "$wrapper_pid" 2>/dev/null || true

  # 2a. Wrapper process must be gone.
  if kill -0 "$wrapper_pid" 2>/dev/null; then
    bad "trap: wrapper process still running after SIGINT"
    kill -9 "$wrapper_pid" 2>/dev/null || true
  else
    ok "trap: wrapper process exited after SIGINT"
  fi

  # 2b. No orphaned stub processes.
  sleep 1
  local orphans=0
  if [ -f "$home/trap-pids" ]; then
    local pid
    while IFS= read -r pid; do
      [ -n "$pid" ] || continue
      if kill -0 "$pid" 2>/dev/null; then
        orphans=$((orphans + 1))
        kill "$pid" 2>/dev/null || true
      fi
    done <"$home/trap-pids"
  fi
  [ "$orphans" -eq 0 ] \
    && ok "trap: no orphaned stub processes after SIGINT (orphans=$orphans)" \
    || bad "trap: $orphans orphaned process(es) survived SIGINT"

  # 2c. The run row must be left in status=running (NOT force-finished) so
  #     resume can abort+retry it on the next run.
  local run_status
  run_status="$(sqlite3 "$db" \
    "select coalesce((select status from runs order by id limit 1), 'absent');" \
    2>/dev/null || printf 'absent')"
  # The row should be 'running' (crash-left) or 'absent' if the cell never
  # opened it (valid too: if SIGINT fired before bench start, there's no row).
  case "$run_status" in
    running|absent)
      ok "trap: run row left in resumable state (status=$run_status, NOT force-finished)" ;;
    completed|aborted)
      bad "trap: run row has status=$run_status — trap force-finished the run (should not)" ;;
    *)
      ok "trap: run row status=$run_status (acceptable for interrupted run)" ;;
  esac

  rm -rf "$home"
}

# ===========================================================================
# TEST 3 — Retry cap: deterministically crashing cell hits cap then PERM_FAIL
# ===========================================================================
test_retry_cap() {
  printf '\n=== M4 TEST 3: per-cell retry cap ===\n'
  local home db cfg p1
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"
  p1="$(seed_plan "$db" "$cfg" "retry cap plan")"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"

  # Wrapper: run_cell always crashes (simulates poison cell).
  # CELL_RETRY_CAP=2 means 2 retries allowed → 3rd crash triggers PERM_FAIL.
  # We run the matrix 3 times and capture output of the 3rd run.
  local wrapper="$home/retry-cap-wrapper.sh"
  cat >"$wrapper" <<RCWRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$REPO_ROOT"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
CELL_RETRY_CAP=2

# Stub run_cell: always crash leaving status=running.
run_cell() {
  local plan="\$1" arm="\$2" rep="\$3"
  local uid="\${CELL_RUN_UID_OVERRIDE}"
  CELL_RUN_UID="\$uid"; CELL_PLAN="\$plan"; CELL_ARM="\$arm"
  _INFLIGHT_CELL_UID="\$uid"
  command "$PLANAR_BIN" bench start "\$uid" --plan "\$plan" --arm "\$arm" \\
    --base-sha stub --config-hash seeded >/dev/null 2>&1 || true
  # Leave status=running (no bench finish) → resume sees crashed mid-cell.
  _INFLIGHT_CELL_UID=""
  exit 1
}
ARG_MATRIX=1
CORPUS_PLANS="$p1"
N_REPS=1
CEILING=999.00
require_tool sqlite3
run_matrix 2>&1
RCWRAP
  chmod +x "$wrapper"

  # Run 1: all 3 arms (strict/eligibility/grouped) crash for the first time.
  bash "$wrapper" >/dev/null 2>&1 || true
  # Run 2: resume sees running rows, aborts them, dispatches retry1 → each crashes again.
  bash "$wrapper" >/dev/null 2>&1 || true
  # Run 3: resume sees retry1 running, aborts, dispatches retry2 → each crashes.
  # This is the 3rd crash; with cap=2 (meaning 2 retries already used), the
  # NEXT run should see PERM_FAIL.
  bash "$wrapper" >/dev/null 2>&1 || true
  # Run 4: should see PERM_FAIL for all arms.
  local out4; out4="$(bash "$wrapper" 2>&1 || true)"

  printf '%s\n' "$out4" | grep -qiE 'PERM_FAIL|retry cap|permanently failed' \
    && ok "retry cap: PERM_FAIL message in output after exceeding cap=2" \
    || bad "retry cap: PERM_FAIL message NOT found in run-4 output (cap=2 not enforced)"

  # 3a. After PERM_FAIL a 5th run must SKIP (not attempt yet another retry).
  local out5; out5="$(bash "$wrapper" 2>&1 || true)"
  # The matrix should skip all cells — either PERM_FAIL or SKIPPED messages.
  printf '%s\n' "$out5" | grep -qiE 'PERM_FAIL|permanently failed|SKIPPED' \
    && ok "retry cap: 5th run skips permanently failed cells (no new dispatch)" \
    || bad "retry cap: 5th run attempted new dispatches for permanently failed cells"

  # 3b. No completed run should exist (the cell always crashed).
  local n_completed
  n_completed="$(sqlite3 "$db" \
    "select count(*) from runs where status='completed';" 2>/dev/null || printf '0')"
  [ "$n_completed" -eq 0 ] \
    && ok "retry cap: zero completed runs (all cells crashed, none force-completed)" \
    || bad "retry cap: $n_completed completed run(s) found (unexpected)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 4 — Structured JSONL logging: written, valid JSON, expected records
# ===========================================================================
test_structured_log() {
  printf '\n=== M4 TEST 4: structured per-cell JSONL logging ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"
  local p; p="$(seed_plan "$db" "$cfg" "log test plan")"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"
  local log_root="$home/cell-logs"

  # Wrapper: run_cell mimics the structured-log pattern (cell_start + cell_complete).
  local wrapper="$home/log-wrapper.sh"
  cat >"$wrapper" <<LOGWRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_LOG_ROOT="$log_root"
export BENCH_CORPUS_REPO="$REPO_ROOT"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$log_root"

run_cell() {
  local plan="\$1" arm="\$2" rep="\$3"
  local uid="\${CELL_RUN_UID_OVERRIDE}"
  CELL_RUN_UID="\$uid"; CELL_PLAN="\$plan"; CELL_ARM="\$arm"
  _INFLIGHT_CELL_UID="\$uid"
  _INFLIGHT_CELL_DIRS=()
  # Emit cell_start (mirrors real run_cell).
  cell_log "cell_start" "\$(jq -nc --arg r "\$rep" '{rep:\$r,base_sha:"stub",tasks:[]}')" || true
  # Write completed bench row.
  command "$PLANAR_BIN" bench start "\$uid" --plan "\$plan" --arm "\$arm" \\
    --base-sha stub --config-hash seeded >/dev/null 2>&1 || true
  command "$PLANAR_BIN" bench finish "\$uid" --status completed >/dev/null 2>&1 || true
  # Emit cell_complete.
  cell_log "cell_complete" '{"disposition":"completed"}' || true
  _INFLIGHT_CELL_UID=""
  _INFLIGHT_CELL_DIRS=()
}
ARG_MATRIX=1
CORPUS_PLANS="$p"
N_REPS=1
CEILING=999.00
require_tool sqlite3
run_matrix 2>&1
LOGWRAP
  chmod +x "$wrapper"
  bash "$wrapper" >/dev/null 2>&1 || true

  # 4a. log_root must exist.
  [ -d "$log_root" ] \
    && ok "structured log: log_root directory created ($log_root)" \
    || bad "structured log: log_root NOT created"

  # 4b. At least one .jsonl file.
  local jsonl_count; jsonl_count="$(find "$log_root" -name '*.jsonl' 2>/dev/null | wc -l | tr -d ' ')"
  [ "$jsonl_count" -ge 1 ] \
    && ok "structured log: ${jsonl_count} .jsonl file(s) found" \
    || bad "structured log: no .jsonl files found"

  # 4c. Every line in every JSONL file must be valid JSON (via jq).
  local invalid_lines=0
  while IFS= read -r logfile; do
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      if ! printf '%s' "$line" | jq '.' >/dev/null 2>&1; then
        invalid_lines=$((invalid_lines + 1))
        bad "structured log: invalid JSON in $logfile: $line"
      fi
    done <"$logfile"
  done < <(find "$log_root" -name '*.jsonl' 2>/dev/null)
  [ "$invalid_lines" -eq 0 ] \
    && ok "structured log: all lines are valid JSON (jq validated)" \
    || bad "structured log: $invalid_lines invalid JSON line(s)"

  # 4d. Required fields: ts, event, run_uid, plan, arm.
  local missing_fields=0
  while IFS= read -r logfile; do
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      for field in ts event run_uid plan arm; do
        if ! printf '%s' "$line" | jq -e "has(\"$field\")" >/dev/null 2>&1; then
          missing_fields=$((missing_fields + 1))
        fi
      done
    done <"$logfile"
  done < <(find "$log_root" -name '*.jsonl' 2>/dev/null)
  [ "$missing_fields" -eq 0 ] \
    && ok "structured log: all records have required fields (ts,event,run_uid,plan,arm)" \
    || bad "structured log: $missing_fields missing field occurrence(s)"

  # 4e. Must find cell_start + cell_complete events.
  local n_start n_complete
  n_start="$(find "$log_root" -name '*.jsonl' 2>/dev/null \
    | xargs grep -l '"event":"cell_start"' 2>/dev/null | wc -l | tr -d ' ')"
  n_complete="$(find "$log_root" -name '*.jsonl' 2>/dev/null \
    | xargs grep -l '"event":"cell_complete"' 2>/dev/null | wc -l | tr -d ' ')"
  [ "$n_start" -ge 1 ] \
    && ok "structured log: found cell_start record(s) in ${n_start} file(s)" \
    || bad "structured log: no cell_start records found"
  [ "$n_complete" -ge 1 ] \
    && ok "structured log: found cell_complete record(s) in ${n_complete} file(s)" \
    || bad "structured log: no cell_complete records found"

  # 4f. --dry-run (DRY_RUN=1) must write NO JSONL files.
  # Test via a wrapper that sources the matrix, sets DRY_RUN=1, and calls cell_log;
  # the log must not write anything.
  local dry_log="$home/dry-logs"
  mkdir -p "$dry_log"
  local dry_state="$home/dry-state"
  local dry_trimmed; dry_trimmed="$(trimmed_matrix "$home/drymatrix-nomain.sh")"
  local dry_wrapper="$home/dry-wrapper.sh"
  cat >"$dry_wrapper" <<DRYW
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_LOG_ROOT="$dry_log"
export BENCH_CORPUS_REPO="$REPO_ROOT"
source "$dry_trimmed"
LOG_ROOT="$dry_log"
DRY_RUN=1
CELL_RUN_UID="dry-test"
CELL_PLAN="$p"
CELL_ARM="strict"
_INFLIGHT_CELL_UID="dry-test"
# cell_log must no-op when DRY_RUN=1
cell_log "cell_start" '{"test":"dry"}' || true
cell_log "cell_complete" '{"disposition":"completed"}' || true
printf 'done\n' >"$dry_state"
DRYW
  chmod +x "$dry_wrapper"
  bash "$dry_wrapper" >/dev/null 2>&1 || true
  local dry_count; dry_count="$(find "$dry_log" -name '*.jsonl' 2>/dev/null | wc -l | tr -d ' ')"
  [ "$dry_count" -eq 0 ] \
    && ok "structured log: DRY_RUN=1 writes 0 JSONL files (cell_log no-ops)" \
    || bad "structured log: DRY_RUN=1 wrote $dry_count JSONL file(s) (expected 0)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 5 — Reviewer-path watchdog: fires + kills process group (pure-bash)
#
# This test exercises spawn_agent_raw (the reviewer spawn path) through the
# watchdog.  If the reviewer were left as a raw `claude -p` call (the M4
# bug), the watchdog would never fire and this test would hang for 20s then
# fail assertion 5c (wall-clock > deadline).
# ===========================================================================
test_reviewer_watchdog() {
  printf '\n=== M4 TEST 5: reviewer-path watchdog (spawn_agent_raw) ===\n'
  local home db cfg
  home="$(new_iso_home)"; db="$home/exp.db"; cfg="$home/config.toml"

  # Build a minimal corpus git repo.
  local corpus="$home/corpus"
  mkdir -p "$corpus"
  git -C "$corpus" init -q
  git -C "$corpus" config user.email t5@test.local
  git -C "$corpus" config user.name m4t5
  printf 'base\n' >"$corpus/seed.txt"
  git -C "$corpus" add -A
  git -C "$corpus" commit -qm base

  # Worktree for the reviewer call.
  local wt="$home/slice-r"
  git -C "$corpus" worktree add -q --detach "$wt" HEAD

  # Slow reviewer stub: sleeps 20s, records own PID + one child PID.
  # The result JSON includes a VERDICT line so verdict parsing can be validated
  # separately (in the AGENT_TIMEOUT=0 sub-case below).
  local stub_dir="$home/stub-dir-r"
  mkdir -p "$stub_dir"
  cat >"$stub_dir/claude" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$\$" >>"${home}/stub-pids-r"
sleep 9999 &
CHILD=\$!
printf '%s\n' "\$CHILD" >>"${home}/stub-pids-r"
sleep 20
kill "\$CHILD" 2>/dev/null || true
printf '%s\n' '{"usage":{"input_tokens":1,"output_tokens":1,"cache_read_input_tokens":0,"cache_creation_input_tokens":0},"total_cost_usd":0.001,"result":"VERDICT: approve\nlooked good"}'
STUB
  chmod +x "$stub_dir/claude"

  local trimmed; trimmed="$(trimmed_matrix "$home/matrix-nomain.sh")"
  local state_file="$home/reviewer-watchdog-state"

  # Wrapper: sources matrix, calls spawn_agent_raw (the reviewer helper) with a
  # slow stub.  AGENT_TIMEOUT=2 → deadline fires long before the 20s stub
  # completes.
  #
  # Timeout detection: spawn_agent_raw is called inside $() so its internal
  # AGENT_TIMED_OUT global and _AGENT_TIMEOUT_FLAG are in a subshell; they
  # cannot be observed directly.  Instead we observe the rc that spawn_agent_raw
  # now propagates on watchdog timeout (non-zero) vs success (0).  The wall-clock
  # check (5c) independently confirms the watchdog fired vs the stub running to
  # 20s completion.
  #
  # This wrapper would HANG for ~20s if spawn_agent_raw didn't route through
  # spawn_agent_watchdog (i.e. if the reviewer were still a raw `claude -p`).
  local wrapper="$home/reviewer-watchdog-wrapper.sh"
  cat >"$wrapper" <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
export PATH="${stub_dir}:\${PATH}"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
AGENT_TIMEOUT=2
DRY_RUN=0
before=\$(date +%s)
rc=0
out="\$(spawn_agent_raw "reviewer-model" "$wt" "review this" 2>/dev/null)" || rc=\$?
after=\$(date +%s)
elapsed=\$(( after - before ))
printf 'rc=%s\n'      "\$rc"      >"$state_file"
printf 'elapsed=%s\n' "\$elapsed" >>"$state_file"
WRAPPER
  chmod +x "$wrapper"

  local before_ts; before_ts="$(date +%s)"
  bash "$wrapper" 2>/dev/null || true
  local after_ts; after_ts="$(date +%s)"
  local wall=$(( after_ts - before_ts ))

  # Read wrapper results.
  local wrc=0 welapsed=0
  if [ -f "$state_file" ]; then
    wrc="$(grep '^rc=' "$state_file" | cut -d= -f2)"
    welapsed="$(grep '^elapsed=' "$state_file" | cut -d= -f2)"
  fi

  # 5a. spawn_agent_raw must propagate non-zero when the watchdog fires.
  #     A raw `claude -p` call would never reach this because it would block
  #     for 20s; if the reviewer were unwrapped, rc would be 0 (stub exit)
  #     or the test would hang past the wall-clock guard below.
  [ "${wrc:-0}" -ne 0 ] \
    && ok "reviewer watchdog: spawn_agent_raw returned non-zero on deadline breach (rc=$wrc)" \
    || bad "reviewer watchdog: returned 0 — watchdog did NOT fire or rc not propagated (rc=$wrc)"

  # 5b. Wall-clock must be close to the deadline, NOT 20s.
  #     This is the definitive proof: a raw claude call (unwrapped reviewer)
  #     would block for the full 20s stub runtime; the watchdog cuts it at ~2s.
  [ "$wall" -le 12 ] \
    && ok "reviewer watchdog: wall-clock ${wall}s <= 12s (watchdog fired, not 20s stub)" \
    || bad "reviewer watchdog: wall-clock ${wall}s > 12s — watchdog too slow or not wired"

  # 5c. No orphaned stub processes (proves process GROUP died).
  sleep 1
  local orphans=0
  if [ -f "$home/stub-pids-r" ]; then
    local pid
    while IFS= read -r pid; do
      [ -n "$pid" ] || continue
      if kill -0 "$pid" 2>/dev/null; then
        orphans=$((orphans + 1))
        kill "$pid" 2>/dev/null || true
      fi
    done <"$home/stub-pids-r"
  fi
  [ "$orphans" -eq 0 ] \
    && ok "reviewer watchdog: no orphaned stub processes after watchdog kill (pids checked)" \
    || bad "reviewer watchdog: $orphans orphaned process(es) survived the watchdog kill"

  # 5d. AGENT_TIMEOUT=0 disables the watchdog on the reviewer path too;
  #     a fast stub must succeed AND its result JSON must be returned verbatim
  #     (preserving the VERDICT line that reviewer_verdict parses).
  local fast_dir="$home/fast-dir-r"
  mkdir -p "$fast_dir"
  cat >"$fast_dir/claude" <<FAST
#!/usr/bin/env bash
printf '%s\n' '{"usage":{"input_tokens":2,"output_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0},"total_cost_usd":0.002,"result":"VERDICT: approve\nlooked good"}'
FAST
  chmod +x "$fast_dir/claude"
  local fast_state="$home/fast-state-r"
  local fast_wrapper="$home/fast-reviewer-wrapper.sh"
  cat >"$fast_wrapper" <<FWRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
export PATH="${fast_dir}:\${PATH}"
# shellcheck disable=SC1090
source "$trimmed"
LOG_ROOT="$home/cell-logs"
AGENT_TIMEOUT=0
DRY_RUN=0
rc=0
out="\$(spawn_agent_raw "reviewer-model" "$wt" "review this" 2>/dev/null)" || rc=\$?
printf 'rc=%s\n' "\$rc" >"$fast_state"
# Check verdict extraction from the raw JSON.
verdict="\$(printf '%s' "\$out" | jq -r '.result // ""' \
  | grep -oiE 'VERDICT:[[:space:]]*(approve|request-changes)' \
  | head -1 | awk '{print tolower(\$2)}' || true)"
printf 'verdict=%s\n' "\$verdict" >>"$fast_state"
# Check token_sample projection from the raw JSON (mirrors run_agents usage).
sample="\$(printf '%s' "\$out" | jq -c '{in:(.usage.input_tokens // 0),out:(.usage.output_tokens // 0),usd:(.total_cost_usd // 0)}' 2>/dev/null || printf 'null')"
printf 'sample=%s\n' "\$sample" >>"$fast_state"
FWRAP
  chmod +x "$fast_wrapper"
  bash "$fast_wrapper" 2>/dev/null || true
  local fast_rc=0 fast_verdict="" fast_sample=""
  if [ -f "$fast_state" ]; then
    fast_rc="$(grep '^rc=' "$fast_state" | cut -d= -f2)"
    fast_verdict="$(grep '^verdict=' "$fast_state" | cut -d= -f2)"
    fast_sample="$(grep '^sample=' "$fast_state" | cut -d= -f2)"
  fi
  [ "${fast_rc:-1}" -eq 0 ] \
    && ok "reviewer watchdog: AGENT_TIMEOUT=0 disables watchdog, fast stub succeeds (rc=0)" \
    || bad "reviewer watchdog: AGENT_TIMEOUT=0 path returned rc=${fast_rc:-?} (expected 0)"
  [ "${fast_verdict:-}" = "approve" ] \
    && ok "reviewer watchdog: reviewer_verdict parses correctly from spawn_agent_raw output (verdict=approve)" \
    || bad "reviewer watchdog: verdict='${fast_verdict:-}' (expected 'approve') — raw JSON not forwarded"
  [ "${fast_sample:-null}" != "null" ] && [ "${fast_sample:-}" != "" ] \
    && ok "reviewer watchdog: token_sample projection works from spawn_agent_raw output (sample=$fast_sample)" \
    || bad "reviewer watchdog: token_sample projection failed (sample='${fast_sample:-}')"

  rm -rf "$home"
}

# ===========================================================================
# TEST 6 — Regression: M3 and B1 tests still pass
# ===========================================================================
test_regression_m3() {
  printf '\n=== M4 TEST 6a: M3 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: M3 tests pass after M4 changes" \
    || bad "regression: M3 tests FAILED (rc=$rc)"
}

test_regression_b1() {
  printf '\n=== M4 TEST 6b: B1 regression ===\n'
  local rc=0
  bash "$SCRIPT_DIR/bench-matrix-b1-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: B1 tests pass after M4 changes" \
    || bad "regression: B1 tests FAILED (rc=$rc)"
}

# ===========================================================================
# main
# ===========================================================================
main() {
  command -v jq      >/dev/null || { echo "jq required"; exit 2; }
  command -v sqlite3 >/dev/null || { echo "sqlite3 required"; exit 2; }
  command -v git     >/dev/null || { echo "git required"; exit 2; }

  test_timeout_watchdog
  test_trap_cleanup
  test_retry_cap
  test_structured_log
  test_reviewer_watchdog
  test_regression_m3
  test_regression_b1

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
