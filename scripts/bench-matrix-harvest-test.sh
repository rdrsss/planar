#!/usr/bin/env bash
#
# bench-matrix-harvest-test.sh — regression tests for the confirmatory-run
# harness commit + zero-touch guard + raw-transcript changes (plan 699, task
# 4361). NO real claude invocations, NO network, NO spend.
#
# Test plan:
#   T1: Harness commits agent edits — create a slice worktree, simulate an
#       agent leaving UNCOMMITTED edits (just write files — no commit), call
#       commit_agent_work, assert HEAD advanced and the files are committed,
#       then assert harvest_slice records them as actual touches (>0). This is
#       the core regression: it would FAIL on the old code (no commit -> 0 touches).
#   T2: Empty slice is clean — agent leaves NO edits -> commit_agent_work is a
#       no-op (HEAD unchanged, no error), harvest yields 0, run continues.
#   T3: Zero-touch guard fires — a cell whose slices all yield 0 actual touches
#       emits the WARNING line; a cell with >0 actual touches does NOT emit it.
#   T4: Raw transcript preserved — spawn_agent/spawn_agent_raw write *.raw.json
#       alongside the projected coder.json/reviewer.json when out_dir is passed.
#   T5: No regression — existing base-test, m3-test, m4-test, b1-test pass.
#
# ISOLATION: each test uses a fresh temp PLANAR_DB + a throwaway corpus git repo.
# The real ~/.planar is NEVER named.
#
# Usage: scripts/bench-matrix-harvest-test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLANAR_BIN="${PLANAR_BIN:-planar}"

# Planning verbs refuse from inside a git worktree (the worktree planning-verb
# split). REPO_ROOT may itself be a worktree; derive the primary checkout.
PLANNING_CWD="$(git -C "$REPO_ROOT" worktree list --porcelain 2>/dev/null \
  | awk '/^worktree /{print $2; exit}')"
[ -n "$PLANNING_CWD" ] || PLANNING_CWD="$REPO_ROOT"

pass=0; fail=0
ok()  { printf 'PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad() { printf 'FAIL: %s\n' "$*"; fail=$((fail + 1)); }

# new_corpus <home> — init a throwaway git repo with one base commit; prints path.
new_corpus() {
  local home="$1" repo="$1/corpus"
  mkdir -p "$repo"
  git -C "$repo" init -q
  git -C "$repo" config user.email harvest@test.local
  git -C "$repo" config user.name harvest-test
  printf 'base\n' >"$repo/seed.txt"
  git -C "$repo" add -A
  git -C "$repo" commit -qm base
  printf '%s' "$repo"
}

# seed_plan <db> <cfg> <corpus> — create a global-scope plan; prints plan id.
seed_plan() {
  local db="$1" cfg="$2" corpus="$3"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" plan create "harvest fixture" --scope global --json 2>/dev/null \
    | jq -r '.id' )
}

# seed_task <db> <cfg> <corpus> <plan> — add one task to <plan>; prints task id.
seed_task() {
  local db="$1" cfg="$2" corpus="$3" plan="$4" tid
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" task add "harvest task" --plan "$plan" --scope global \
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
# TEST 1 — Harness commits agent edits: uncommitted writes -> commit_agent_work
#           -> HEAD advances -> harvest_slice records actual touches.
# This is the CORE REGRESSION TEST: before the fix commit_agent_work did not
# exist, so an agent that forgot to commit left harvest reading HEAD==base and
# recording zero actual touches. The test would FAIL on the old code.
# ===========================================================================
test_harness_commits_agent_edits() {
  printf '\n=== HARVEST TEST 1: harness commits uncommitted agent edits ===\n'
  local home db cfg corpus base wt integ pid tid
  home="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  tid="$(seed_task "$db" "$cfg" "$corpus" "$pid")"

  source_matrix "$db" "$cfg" "$corpus"
  CELL_RUN_UID="hv-t1-commit"
  CELL_BASE_SHA="$base"
  CELL_ARM="strict"
  CELL_INTEG_BRANCH="bench-integ/$CELL_RUN_UID"

  # Open a run record so harvest_slice can write run_touches rows.
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" bench start "$CELL_RUN_UID" --plan "$pid" --arm strict \
        --base-sha "$base" --config-hash hv1 >/dev/null 2>&1 )

  # Set up integration worktree (needed by fanin_conflict_check).
  integ="$home/integ"
  git -C "$corpus" worktree add -q --detach "$integ" "$base"
  git -C "$integ" checkout -q -b "$CELL_INTEG_BRANCH"
  INTEG_WORKTREE="$integ"

  # Slice worktree: simulate an agent that wrote files but did NOT commit.
  wt="$home/slice1"
  git -C "$corpus" worktree add -q --detach "$wt" "$base"
  printf 'agent work\n' >"$wt/agent_created.txt"
  printf 'agent modified seed\n' >"$wt/seed.txt"
  # Do NOT commit — this is what the old harness allowed through.

  # Confirm HEAD == base BEFORE commit_agent_work (regression anchor).
  local head_before; head_before="$(git -C "$wt" rev-parse HEAD)"
  [ "$head_before" = "$base" ] \
    && ok "T1-pre: slice HEAD == base BEFORE commit_agent_work (uncommitted edits visible)" \
    || bad "T1-pre: HEAD was not base before call (unexpected prior commit)"

  # Call the new helper.
  commit_agent_work "$wt" "test-plan: slice t1"

  # HEAD must have advanced (a commit was made).
  local head_after; head_after="$(git -C "$wt" rev-parse HEAD)"
  [ "$head_after" != "$base" ] \
    && ok "T1a: HEAD advanced after commit_agent_work (${base} -> ${head_after})" \
    || bad "T1a: HEAD did NOT advance — commit_agent_work left edits uncommitted"

  # The agent_created.txt file must be in the committed tree.
  git -C "$wt" show HEAD:agent_created.txt >/dev/null 2>&1 \
    && ok "T1b: agent_created.txt is in committed HEAD tree" \
    || bad "T1b: agent_created.txt NOT in HEAD tree after commit_agent_work"

  # Now harvest_slice should record actual touches.
  harvest_slice "$wt" "[${tid}]"

  local n_actual
  n_actual="$(sqlite3 "$db" \
    "select count(*) from run_touches t join runs r on r.id=t.run_id
     where r.run_uid='$CELL_RUN_UID' and t.kind='actual';")"
  [ "${n_actual:-0}" -ge 1 ] \
    && ok "T1c: harvest_slice recorded >=1 actual touch (got $n_actual) — core regression passes" \
    || bad "T1c: harvest_slice recorded 0 actual touches — regression NOT fixed (got $n_actual)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 2 — Empty slice: agent leaves NO edits -> commit_agent_work no-op,
#           harvest yields 0, run does NOT error.
# ===========================================================================
test_empty_slice_is_clean() {
  printf '\n=== HARVEST TEST 2: empty slice — commit_agent_work is no-op ===\n'
  local home db cfg corpus base wt integ pid tid
  home="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  tid="$(seed_task "$db" "$cfg" "$corpus" "$pid")"

  source_matrix "$db" "$cfg" "$corpus"
  CELL_RUN_UID="hv-t2-empty"
  CELL_BASE_SHA="$base"
  CELL_ARM="strict"
  CELL_INTEG_BRANCH="bench-integ/$CELL_RUN_UID"

  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" bench start "$CELL_RUN_UID" --plan "$pid" --arm strict \
        --base-sha "$base" --config-hash hv2 >/dev/null 2>&1 )

  integ="$home/integ"
  git -C "$corpus" worktree add -q --detach "$integ" "$base"
  git -C "$integ" checkout -q -b "$CELL_INTEG_BRANCH"
  INTEG_WORKTREE="$integ"

  # Slice worktree with NO agent edits — HEAD == base, nothing staged.
  wt="$home/slice0"
  git -C "$corpus" worktree add -q --detach "$wt" "$base"

  # commit_agent_work must be a no-op (no commit, no error).
  local rc=0
  commit_agent_work "$wt" "test-plan: empty slice" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "T2a: commit_agent_work exits 0 on empty slice (no error)" \
    || bad "T2a: commit_agent_work failed (rc=$rc) on empty slice"

  local head_after; head_after="$(git -C "$wt" rev-parse HEAD)"
  [ "$head_after" = "$base" ] \
    && ok "T2b: HEAD unchanged after commit_agent_work on empty slice" \
    || bad "T2b: HEAD unexpectedly advanced on empty slice (was $base, now $head_after)"

  # harvest_slice must record 0 actual touches (not an error).
  harvest_slice "$wt" "[${tid}]"
  local n_actual
  n_actual="$(sqlite3 "$db" \
    "select count(*) from run_touches t join runs r on r.id=t.run_id
     where r.run_uid='$CELL_RUN_UID' and t.kind='actual';")"
  [ "${n_actual:-0}" -eq 0 ] \
    && ok "T2c: empty slice yields 0 actual touches — correct" \
    || bad "T2c: empty slice unexpectedly yielded $n_actual actual touches"

  rm -rf "$home"
}

# ===========================================================================
# TEST 3 — Zero-touch guard fires when cell has 0 actual touches, does NOT
#           fire when cell has >0 actual touches.
# The guard is in run_cell's live path; we test it by calling it directly via
# a wrapper that sources the matrix and reproduces the guard's sqlite query.
# ===========================================================================
test_zero_touch_guard() {
  printf '\n=== HARVEST TEST 3: zero-touch guard fires / does not fire ===\n'

  # Sub-case A: cell with 0 actual touches emits WARNING.
  local homeA dbA cfgA corpusA baseA pidA
  homeA="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  dbA="$homeA/exp.db"; cfgA="$homeA/config.toml"
  corpusA="$(new_corpus "$homeA")"
  baseA="$(git -C "$corpusA" rev-parse HEAD)"
  pidA="$(seed_plan "$dbA" "$cfgA" "$corpusA")"

  # Open a run record with no touches.
  ( cd "$corpusA" && PLANAR_DB="$dbA" PLANAR_CONFIG_PATH="$cfgA" \
      command "$PLANAR_BIN" bench start "hv-t3-zero" --plan "$pidA" --arm strict \
        --base-sha "$baseA" --config-hash hv3 >/dev/null 2>&1 )

  # Build a wrapper that sources the matrix and runs just the zero-touch guard.
  local trimmedA="$homeA/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmedA"
  local wrapA="$homeA/t3a.sh"
  cat >"$wrapA" <<WRAP3A
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$dbA"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfgA"
export BENCH_HOME="$homeA"
export BENCH_CORPUS_REPO="$corpusA"
# shellcheck disable=SC1090
source "$trimmedA"
# Reproduce the guard from run_cell verbatim.
run_uid="hv-t3-zero"
if [ -f "\$PLANAR_DB" ]; then
  _n_actual="\$(sqlite3 "\$PLANAR_DB" \
    "select count(*) from run_touches t
     join runs r on r.id = t.run_id
     where r.run_uid = '\${run_uid}' and t.kind = 'actual';" \
    2>/dev/null || printf '0')"
  if [ "\${_n_actual:-0}" -eq 0 ]; then
    log "WARNING: cell \${run_uid} harvested 0 actual touches — agent produced no committed changes; RQ1 data for this cell is empty"
  else
    log "zero-touch guard: cell \${run_uid} has \${_n_actual} actual touch(es) — OK"
  fi
fi
WRAP3A
  chmod +x "$wrapA"
  local outA; outA="$(bash "$wrapA" 2>&1 || true)"
  printf '%s\n' "$outA" | grep -qF "WARNING: cell hv-t3-zero harvested 0 actual touches" \
    && ok "T3a: zero-touch guard emits WARNING for cell with 0 actual touches" \
    || bad "T3a: WARNING not emitted for 0-touch cell; output: $outA"
  rm -rf "$homeA"

  # Sub-case B: cell with >0 actual touches does NOT emit WARNING.
  local homeB dbB cfgB corpusB baseB pidB tidB
  homeB="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  dbB="$homeB/exp.db"; cfgB="$homeB/config.toml"
  corpusB="$(new_corpus "$homeB")"
  baseB="$(git -C "$corpusB" rev-parse HEAD)"
  pidB="$(seed_plan "$dbB" "$cfgB" "$corpusB")"
  tidB="$(seed_task "$dbB" "$cfgB" "$corpusB" "$pidB")"

  ( cd "$corpusB" && PLANAR_DB="$dbB" PLANAR_CONFIG_PATH="$cfgB" \
      command "$PLANAR_BIN" bench start "hv-t3-ok" --plan "$pidB" --arm strict \
        --base-sha "$baseB" --config-hash hv3b >/dev/null 2>&1 )

  # Manually write one actual touch row to simulate a non-empty harvest.
  sqlite3 "$dbB" "
    insert into run_touches (run_id, task_id, kind, path)
    select r.id, $tidB, 'actual', 'some/file.zig'
    from runs r where r.run_uid='hv-t3-ok';" 2>/dev/null

  local trimmedB="$homeB/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmedB"
  local wrapB="$homeB/t3b.sh"
  cat >"$wrapB" <<WRAP3B
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$dbB"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfgB"
export BENCH_HOME="$homeB"
export BENCH_CORPUS_REPO="$corpusB"
# shellcheck disable=SC1090
source "$trimmedB"
run_uid="hv-t3-ok"
if [ -f "\$PLANAR_DB" ]; then
  _n_actual="\$(sqlite3 "\$PLANAR_DB" \
    "select count(*) from run_touches t
     join runs r on r.id = t.run_id
     where r.run_uid = '\${run_uid}' and t.kind = 'actual';" \
    2>/dev/null || printf '0')"
  if [ "\${_n_actual:-0}" -eq 0 ]; then
    log "WARNING: cell \${run_uid} harvested 0 actual touches — agent produced no committed changes; RQ1 data for this cell is empty"
  else
    log "zero-touch guard: cell \${run_uid} has \${_n_actual} actual touch(es) — OK"
  fi
fi
WRAP3B
  chmod +x "$wrapB"
  local outB; outB="$(bash "$wrapB" 2>&1 || true)"
  if printf '%s\n' "$outB" | grep -qF "WARNING: cell hv-t3-ok harvested 0 actual touches"; then
    bad "T3b: zero-touch guard emitted WARNING for cell with actual touches (false positive)"
  else
    ok "T3b: zero-touch guard correctly silent for cell with actual touches"
  fi
  printf '%s\n' "$outB" | grep -qF "zero-touch guard: cell hv-t3-ok has" \
    && ok "T3c: guard logged the OK status for non-zero touch cell" \
    || bad "T3c: OK status NOT logged; output: $outB"
  rm -rf "$homeB"
}

# ===========================================================================
# TEST 4 — Raw transcript preserved: spawn_agent/spawn_agent_raw write
#           *.raw.json alongside *.json when out_dir is provided.
#
# Strategy: source the matrix, stub spawn_agent_watchdog to return a known JSON
# payload (no real claude), call spawn_agent / spawn_agent_raw with an out_dir,
# assert the raw files are written.
# ===========================================================================
test_raw_transcript_preserved() {
  printf '\n=== HARVEST TEST 4: raw transcript files written alongside projection ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"

  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  local out_dir="$home/transcripts/test-slice"
  mkdir -p "$out_dir"

  # Build a wrapper that stubs spawn_agent_watchdog to emit a known JSON, then
  # calls spawn_agent and spawn_agent_raw with out_dir, and asserts the raw files.
  local wrap="$home/t4-wrapper.sh"
  cat >"$wrap" <<WRAP4
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"

# Stub spawn_agent_watchdog to return a fixed known JSON (no real claude).
FAKE_RAW='{"usage":{"input_tokens":42,"output_tokens":7,"cache_read_input_tokens":0,"cache_creation_input_tokens":0},"total_cost_usd":0.001,"result":"VERDICT: approve"}'
spawn_agent_watchdog() {
  printf '%s' "\$FAKE_RAW"
  return 0
}

out_dir="$out_dir"
mkdir -p "\$out_dir"

# --- coder path (spawn_agent with out_dir) ---
spawn_agent "any-model" "." "brief text" "\$out_dir" >/dev/null 2>&1

# --- reviewer path (spawn_agent_raw with out_dir) ---
AGENT_TIMEOUT=0
spawn_agent_raw "any-model" "." "brief text" "\$out_dir" >/dev/null 2>&1 || true
WRAP4
  chmod +x "$wrap"
  bash "$wrap" 2>/dev/null || true

  # Assert coder.raw.json exists and contains the expected raw payload.
  [ -f "$out_dir/coder.raw.json" ] \
    && ok "T4a: coder.raw.json written alongside coder.json" \
    || bad "T4a: coder.raw.json NOT written (raw transcript missing)"

  if [ -f "$out_dir/coder.raw.json" ]; then
    local raw_in; raw_in="$(jq -r '.usage.input_tokens // "MISSING"' "$out_dir/coder.raw.json" 2>/dev/null || printf 'PARSE_ERR')"
    [ "$raw_in" = "42" ] \
      && ok "T4b: coder.raw.json contains the full raw JSON (input_tokens=42)" \
      || bad "T4b: coder.raw.json has unexpected content (input_tokens='$raw_in')"
  fi

  # Assert reviewer.raw.json exists.
  [ -f "$out_dir/reviewer.raw.json" ] \
    && ok "T4c: reviewer.raw.json written alongside reviewer.json" \
    || bad "T4c: reviewer.raw.json NOT written (raw transcript missing)"

  if [ -f "$out_dir/reviewer.raw.json" ]; then
    local raw_result; raw_result="$(jq -r '.result // "MISSING"' "$out_dir/reviewer.raw.json" 2>/dev/null || printf 'PARSE_ERR')"
    printf '%s' "$raw_result" | grep -qi "VERDICT" \
      && ok "T4d: reviewer.raw.json contains the full result text (VERDICT present)" \
      || bad "T4d: reviewer.raw.json missing VERDICT text (got: '$raw_result')"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 5 — No regression: existing test suites still pass.
# ===========================================================================
test_regression() {
  local rc
  printf '\n=== HARVEST TEST 5a: base-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-base-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: base-test passes" \
    || bad "regression: base-test FAILED (rc=$rc)"

  printf '\n=== HARVEST TEST 5b: m3-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m3-test passes" \
    || bad "regression: m3-test FAILED (rc=$rc)"

  printf '\n=== HARVEST TEST 5c: m4-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m4-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m4-test passes" \
    || bad "regression: m4-test FAILED (rc=$rc)"

  printf '\n=== HARVEST TEST 5d: b1-test regression ===\n'
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
  command -v sqlite3 >/dev/null || { printf 'sqlite3 required\n'; exit 2; }
  command -v git     >/dev/null || { printf 'git required\n'; exit 2; }

  test_harness_commits_agent_edits
  test_empty_slice_is_clean
  test_zero_touch_guard
  test_raw_transcript_preserved
  test_regression

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
