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
# Task 4362: T3 now calls the real zero_touch_guard function (factored out of
# run_cell) instead of reproducing the guard's SQL+message in a heredoc. This
# means wording or SQL drift in the real guard would cause this test to fail —
# the test pins the real implementation, not a copy.
# ===========================================================================
test_zero_touch_guard() {
  printf '\n=== HARVEST TEST 3: zero-touch guard fires / does not fire (calls real zero_touch_guard) ===\n'

  # Sub-case A: cell with 0 actual touches — zero_touch_guard must emit WARNING.
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

  # Build a wrapper that sources the matrix and calls zero_touch_guard directly.
  # This is the key difference from the old T3: we call the REAL function,
  # not a heredoc copy — so a SQL or message change in bench-matrix.sh is caught.
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
# Call the real zero_touch_guard function (not a copy of its SQL/message).
zero_touch_guard "hv-t3-zero"
WRAP3A
  chmod +x "$wrapA"
  local outA; outA="$(bash "$wrapA" 2>&1 || true)"
  printf '%s\n' "$outA" | grep -qF "WARNING: cell hv-t3-zero harvested 0 actual touches" \
    && ok "T3a: zero_touch_guard emits WARNING for cell with 0 actual touches" \
    || bad "T3a: WARNING not emitted for 0-touch cell; output: $outA"
  rm -rf "$homeA"

  # Sub-case B: cell with >0 actual touches — zero_touch_guard must NOT emit WARNING.
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
# Call the real zero_touch_guard function.
zero_touch_guard "hv-t3-ok"
WRAP3B
  chmod +x "$wrapB"
  local outB; outB="$(bash "$wrapB" 2>&1 || true)"
  if printf '%s\n' "$outB" | grep -qF "WARNING: cell hv-t3-ok harvested 0 actual touches"; then
    bad "T3b: zero_touch_guard emitted WARNING for cell with actual touches (false positive)"
  else
    ok "T3b: zero_touch_guard correctly silent for cell with actual touches"
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
# TEST 5 — filter_noise_touches: noise rows deleted, real + declared rows kept.
#
# Strategy: seed run_touches with a mix of real actual rows, noise actual rows,
# and declared rows; call filter_noise_touches; assert noise actuals gone, real
# actual kept, declared rows untouched.
# ===========================================================================
test_filter_noise_touches() {
  printf '\n=== HARVEST TEST 5: filter_noise_touches removes noise, keeps real ===\n'
  local home db cfg corpus base pid tid
  home="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  tid="$(seed_task "$db" "$cfg" "$corpus" "$pid")"

  # Open a run record.
  local run_uid="fn-t5-filter"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" bench start "$run_uid" --plan "$pid" --arm strict \
        --base-sha "$base" --config-hash fn5 >/dev/null 2>&1 )

  # Seed run_touches: real actual, noise actuals, declared.
  sqlite3 "$db" "
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tid, 'actual', 'src/foo.zig'       FROM runs r WHERE r.run_uid='$run_uid';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tid, 'actual', 'src/x.zig.bak'     FROM runs r WHERE r.run_uid='$run_uid';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tid, 'actual', 'vendor/lib/y.zig'  FROM runs r WHERE r.run_uid='$run_uid';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tid, 'actual', 'zig-out/bin/z'     FROM runs r WHERE r.run_uid='$run_uid';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tid, 'actual', 'helper.o'           FROM runs r WHERE r.run_uid='$run_uid';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tid, 'declared', 'src/foo.zig'     FROM runs r WHERE r.run_uid='$run_uid';
  " 2>/dev/null

  # Source the matrix and call filter_noise_touches.
  local trimmed="$db.matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"

  local wrap="$home/t5-wrap.sh"
  cat >"$wrap" <<WRAP5
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
filter_noise_touches "$run_uid"
WRAP5
  chmod +x "$wrap"
  bash "$wrap" 2>/dev/null || true

  # Assert: noise actual rows deleted.
  local n_bak n_vendor n_zigout n_dotO
  n_bak="$(sqlite3 "$db" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='actual' AND t.path='src/x.zig.bak';")"
  n_vendor="$(sqlite3 "$db" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='actual' AND t.path='vendor/lib/y.zig';")"
  n_zigout="$(sqlite3 "$db" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='actual' AND t.path='zig-out/bin/z';")"
  n_dotO="$(sqlite3 "$db" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='actual' AND t.path='helper.o';")"

  [ "${n_bak:-1}" -eq 0 ] \
    && ok "T5a: .bak actual touch deleted by filter_noise_touches" \
    || bad "T5a: .bak actual touch NOT deleted (count=$n_bak)"
  [ "${n_vendor:-1}" -eq 0 ] \
    && ok "T5b: vendor/ actual touch deleted by filter_noise_touches" \
    || bad "T5b: vendor/ actual touch NOT deleted (count=$n_vendor)"
  [ "${n_zigout:-1}" -eq 0 ] \
    && ok "T5c: zig-out/ actual touch deleted by filter_noise_touches" \
    || bad "T5c: zig-out/ actual touch NOT deleted (count=$n_zigout)"
  [ "${n_dotO:-1}" -eq 0 ] \
    && ok "T5d: .o actual touch deleted by filter_noise_touches" \
    || bad "T5d: .o actual touch NOT deleted (count=$n_dotO)"

  # Assert: real actual row kept.
  local n_real
  n_real="$(sqlite3 "$db" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='actual' AND t.path='src/foo.zig';")"
  [ "${n_real:-0}" -eq 1 ] \
    && ok "T5e: src/foo.zig real actual touch preserved" \
    || bad "T5e: src/foo.zig actual touch was unexpectedly removed (count=$n_real)"

  # Assert: declared row untouched.
  local n_decl
  n_decl="$(sqlite3 "$db" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='declared' AND t.path='src/foo.zig';")"
  [ "${n_decl:-0}" -eq 1 ] \
    && ok "T5f: declared row untouched by filter_noise_touches" \
    || bad "T5f: declared row was deleted (count=$n_decl)"

  rm -rf "$home"
}

# ===========================================================================
# TEST 6 — BENCH_HARVEST_EXCLUDE override changes what's filtered.
#
# Seed actual rows for .bak and src/real.zig. Run with an override that only
# excludes *.bak. Assert .bak deleted and src/real.zig kept.
# Then re-seed and run with BENCH_HARVEST_EXCLUDE="" (disabled). Assert nothing
# deleted.
# ===========================================================================
test_filter_exclude_override() {
  printf '\n=== HARVEST TEST 6: BENCH_HARVEST_EXCLUDE override changes filter ===\n'

  # Sub-case A: override to only *.bak — vendor/ row is NOT filtered.
  local homeA dbA cfgA corpusA baseA pidA tidA run_uidA
  homeA="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  dbA="$homeA/exp.db"; cfgA="$homeA/config.toml"
  corpusA="$(new_corpus "$homeA")"
  baseA="$(git -C "$corpusA" rev-parse HEAD)"
  pidA="$(seed_plan "$dbA" "$cfgA" "$corpusA")"
  tidA="$(seed_task "$dbA" "$cfgA" "$corpusA" "$pidA")"
  run_uidA="fn-t6a-override"
  ( cd "$corpusA" && PLANAR_DB="$dbA" PLANAR_CONFIG_PATH="$cfgA" \
      command "$PLANAR_BIN" bench start "$run_uidA" --plan "$pidA" --arm strict \
        --base-sha "$baseA" --config-hash fn6a >/dev/null 2>&1 )
  sqlite3 "$dbA" "
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tidA, 'actual', 'src/x.zig.bak'    FROM runs r WHERE r.run_uid='$run_uidA';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tidA, 'actual', 'vendor/lib/y.zig'  FROM runs r WHERE r.run_uid='$run_uidA';
  " 2>/dev/null

  local trimmedA="$dbA.matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmedA"
  local wrapA="$homeA/t6a-wrap.sh"
  cat >"$wrapA" <<WRAP6A
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$dbA"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfgA"
export BENCH_HOME="$homeA"
export BENCH_CORPUS_REPO="$corpusA"
export BENCH_HARVEST_EXCLUDE="*.bak"
# shellcheck disable=SC1090
source "$trimmedA"
filter_noise_touches "$run_uidA"
WRAP6A
  chmod +x "$wrapA"
  bash "$wrapA" 2>/dev/null || true

  local n_bak n_vendor
  n_bak="$(sqlite3 "$dbA" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uidA' AND t.kind='actual' AND t.path='src/x.zig.bak';")"
  n_vendor="$(sqlite3 "$dbA" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uidA' AND t.kind='actual' AND t.path='vendor/lib/y.zig';")"
  [ "${n_bak:-1}" -eq 0 ] \
    && ok "T6a: *.bak deleted when BENCH_HARVEST_EXCLUDE=*.bak" \
    || bad "T6a: *.bak NOT deleted with override (count=$n_bak)"
  [ "${n_vendor:-0}" -eq 1 ] \
    && ok "T6b: vendor/ NOT deleted when override only lists *.bak" \
    || bad "T6b: vendor/ unexpectedly deleted with *.bak-only override (count=$n_vendor)"
  rm -rf "$homeA"

  # Sub-case B: BENCH_HARVEST_EXCLUDE="" — nothing filtered.
  local homeB dbB cfgB corpusB baseB pidB tidB run_uidB
  homeB="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  dbB="$homeB/exp.db"; cfgB="$homeB/config.toml"
  corpusB="$(new_corpus "$homeB")"
  baseB="$(git -C "$corpusB" rev-parse HEAD)"
  pidB="$(seed_plan "$dbB" "$cfgB" "$corpusB")"
  tidB="$(seed_task "$dbB" "$cfgB" "$corpusB" "$pidB")"
  run_uidB="fn-t6b-disabled"
  ( cd "$corpusB" && PLANAR_DB="$dbB" PLANAR_CONFIG_PATH="$cfgB" \
      command "$PLANAR_BIN" bench start "$run_uidB" --plan "$pidB" --arm strict \
        --base-sha "$baseB" --config-hash fn6b >/dev/null 2>&1 )
  sqlite3 "$dbB" "
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tidB, 'actual', 'src/x.zig.bak'    FROM runs r WHERE r.run_uid='$run_uidB';
    INSERT INTO run_touches (run_id, task_id, kind, path)
    SELECT r.id, $tidB, 'actual', 'vendor/lib/y.zig'  FROM runs r WHERE r.run_uid='$run_uidB';
  " 2>/dev/null

  local trimmedB="$dbB.matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmedB"
  local wrapB="$homeB/t6b-wrap.sh"
  cat >"$wrapB" <<WRAP6B
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$dbB"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfgB"
export BENCH_HOME="$homeB"
export BENCH_CORPUS_REPO="$corpusB"
export BENCH_HARVEST_EXCLUDE=""
# shellcheck disable=SC1090
source "$trimmedB"
filter_noise_touches "$run_uidB"
WRAP6B
  chmod +x "$wrapB"
  bash "$wrapB" 2>/dev/null || true

  local n_total
  n_total="$(sqlite3 "$dbB" "SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uidB' AND t.kind='actual';")"
  [ "${n_total:-0}" -eq 2 ] \
    && ok "T6c: BENCH_HARVEST_EXCLUDE='' disables filter — all actual rows preserved" \
    || bad "T6c: BENCH_HARVEST_EXCLUDE='' did not disable filter (remaining actual count=$n_total)"
  rm -rf "$homeB"
}

# ===========================================================================
# TEST 7 — coder_brief contains the no-backup / stay-in-subsystem directive.
# ===========================================================================
test_coder_brief_directive() {
  printf '\n=== HARVEST TEST 7: coder_brief no-backup scope directive ===\n'
  local home db cfg corpus
  home="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"

  local trimmed="$db.matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"
  local wrap="$home/t7-wrap.sh"
  cat >"$wrap" <<WRAP7
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
# Emit coder_brief on stdout so the test can grep it.
coder_brief 699 "test problem statement" '[42]'
WRAP7
  chmod +x "$wrap"
  local brief_out
  brief_out="$(bash "$wrap" 2>/dev/null || true)"

  printf '%s\n' "$brief_out" | grep -qF ".bak" \
    && ok "T7a: coder_brief mentions .bak (backup file prohibition)" \
    || bad "T7a: coder_brief does NOT mention .bak"
  printf '%s\n' "$brief_out" | grep -qiF "backup" \
    && ok "T7b: coder_brief mentions backup (the no-backup directive)" \
    || bad "T7b: coder_brief does NOT contain 'backup'"
  printf '%s\n' "$brief_out" | grep -qF "vendor/" \
    && ok "T7c: coder_brief mentions vendor/ (scope discipline)" \
    || bad "T7c: coder_brief does NOT mention vendor/"
  printf '%s\n' "$brief_out" | grep -qi "SCOPE DISCIPLINE" \
    && ok "T7d: coder_brief contains SCOPE DISCIPLINE section header" \
    || bad "T7d: coder_brief missing SCOPE DISCIPLINE header"

  rm -rf "$home"
}

# ===========================================================================
# TEST 8 — Task 4355: ITER_CAP / config_hash comment accuracy.
# Asserts that:
#   (a) the header and comments no longer claim a request-changes retry loop
#       (the single-verdict model is correctly documented)
#   (b) ITER_CAP is still included in the config_hash input (hash stability)
# ===========================================================================
test_iter_cap_comment_accuracy() {
  printf '\n=== HARVEST TEST 8: task 4355 — ITER_CAP comment accuracy ===\n'

  # (a) The header should NOT claim "retry loop" in the context of ITER_CAP /
  # iteration cap / reviewer. The old phrasing said "verdict gates, iteration
  # cap 5" implying the reviewer enforced retries. Check the reviewer spawn line.
  local reviewer_spawn_line
  reviewer_spawn_line="$(grep -n 'spawn_agent reviewer' "$MATRIX" || true)"
  if printf '%s\n' "$reviewer_spawn_line" | grep -qF "iteration cap 5"; then
    bad "T8a: header still says 'iteration cap 5' on reviewer spawn line — misleading retry-loop claim"
  else
    ok "T8a: header does NOT claim 'iteration cap 5' on reviewer spawn line"
  fi

  # (b) ITER_CAP must still appear in the config_hash_base input string (hash
  # stability: removing it would break pairing with prior cells).
  grep -A5 'config_hash_base()' "$MATRIX" | grep -qF 'ITER_CAP' \
    && ok "T8b: ITER_CAP still in config_hash_base input (hash stability preserved)" \
    || bad "T8b: ITER_CAP NOT found in config_hash_base — hash stability broken"

  # (c) The ITER_CAP comment block must mention that there is no retry loop.
  # The phrase appears across comment lines (not on a single line with ITER_CAP),
  # so search the whole file for the phrase rather than anchoring to ITER_CAP.
  grep -qiF "no request-changes retry loop" "$MATRIX" \
    && ok "T8c: ITER_CAP comment documents 'no request-changes retry loop'" \
    || bad "T8c: ITER_CAP comment does not document single-verdict model (missing 'no request-changes retry loop')"

  # (d) The config_hash_base site must have a code comment explaining ITER_CAP
  # is kept for hash stability (not for a retry counter).
  grep -B2 'config_hash_base()' "$MATRIX" | grep -qiF "hash stability" \
    || grep -A15 '# config_hash_base' "$MATRIX" | grep -qiF "hash stability" \
    && ok "T8d: config_hash_base has 'hash stability' comment explaining ITER_CAP retention" \
    || bad "T8d: config_hash_base missing 'hash stability' explanation for ITER_CAP"
}

# ===========================================================================
# TEST 9 — Task 4356: --tasks subset scopes bench start declared snapshot.
# Asserts that when the ritual's bench start is called with --task <id> flags
# (the fix in bench_run_ritual.lua), only the subset-task declared touches are
# snapshotted. We test this by:
#   (a) Initializing a real project (planar init) to get a valid projects.id.
#   (b) Seeding task_touch_paths directly for two tasks using projects.id FK.
#   (c) Running bench start with --task scoped to only ONE task.
#   (d) Asserting the declared snapshot contains only that task's rows.
#   (e) Baseline: bench start without --task includes both tasks' declared rows.
# ===========================================================================
test_tasks_subset_scopes_declared_snapshot() {
  printf '\n=== HARVEST TEST 9: task 4356 — --tasks subset scopes declared snapshot ===\n'
  local home db cfg corpus base pid t1 t2
  home="$(mktemp -d "${TMPDIR:-/tmp}/harvest-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"

  # Register the corpus as a project (creates a projects.id for FK use).
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" init --json >/dev/null 2>&1 ) || true

  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  t1="$(seed_task "$db" "$cfg" "$corpus" "$pid")"
  t2="$(seed_task "$db" "$cfg" "$corpus" "$pid")"

  # Seed task_touch_paths via direct SQL (task touches add requires assoc/scope
  # plumbing that is out of scope for this isolation test). We use the project
  # row that `planar init` just created to satisfy the repo_id FK.
  sqlite3 "$db" "
    INSERT INTO task_touch_paths (task_id, repo_id, path)
    SELECT $t1, p.id, 'src/task1.zig' FROM projects p LIMIT 1;
    INSERT INTO task_touch_paths (task_id, repo_id, path)
    SELECT $t2, p.id, 'src/task2.zig' FROM projects p LIMIT 1;
  " 2>/dev/null

  # Open a run scoped to ONLY task t1 via --task (the 4356 fix: ritual passes
  # --task for each id in ctx.args.tasks). Only t1's declared path should appear.
  local run_uid="hv-t9-subset"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" bench start "$run_uid" \
        --plan "$pid" --arm strict \
        --base-sha "$base" --config-hash hv9 \
        --task "$t1" >/dev/null 2>&1 )

  # Assert: declared touches for the run include ONLY t1's path.
  local n_t1_decl n_t2_decl
  n_t1_decl="$(sqlite3 "$db" "
    SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='declared' AND t.task_id=$t1;")"
  n_t2_decl="$(sqlite3 "$db" "
    SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid' AND t.kind='declared' AND t.task_id=$t2;")"

  [ "${n_t1_decl:-0}" -ge 1 ] \
    && ok "T9a: declared snapshot includes task ${t1}'s touch (${n_t1_decl} row(s))" \
    || bad "T9a: declared snapshot missing task ${t1}'s touch (got ${n_t1_decl})"

  [ "${n_t2_decl:-0}" -eq 0 ] \
    && ok "T9b: declared snapshot correctly excludes task ${t2}'s touch (subset scoping works)" \
    || bad "T9b: declared snapshot wrongly includes task ${t2}'s touch (${n_t2_decl} row(s)) — subset not scoped"

  # Baseline: bench start WITHOUT --task must snapshot BOTH tasks' declared touches.
  local run_uid_full="hv-t9-full"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" bench start "$run_uid_full" \
        --plan "$pid" --arm strict \
        --base-sha "$base" --config-hash hv9f >/dev/null 2>&1 )

  local n_full_decl
  n_full_decl="$(sqlite3 "$db" "
    SELECT count(*) FROM run_touches t JOIN runs r ON r.id=t.run_id
    WHERE r.run_uid='$run_uid_full' AND t.kind='declared';")"
  [ "${n_full_decl:-0}" -ge 2 ] \
    && ok "T9c: full-plan bench start snapshots both tasks' touches (${n_full_decl} declared rows — baseline)" \
    || bad "T9c: full-plan bench start only snapshotted ${n_full_decl} declared rows (expected >=2)"

  # Source-level: the ritual Lua file must now pass --task flags.
  local ritual_lua; ritual_lua="$(cd "$(dirname "$MATRIX")/.." && pwd)/workflows/bench_run_ritual.lua"
  if [ -f "$ritual_lua" ]; then
    grep -qF '"--task"' "$ritual_lua" \
      && ok "T9d: bench_run_ritual.lua passes --task flags to bench start (4356 fix present)" \
      || bad "T9d: bench_run_ritual.lua does NOT pass --task flags — 4356 fix missing"
  else
    ok "T9d: bench_run_ritual.lua not found at expected path — skipping source check"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 10 — No regression: existing test suites still pass.
# ===========================================================================
test_regression() {
  local rc
  printf '\n=== HARVEST TEST 10a: base-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-base-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: base-test passes" \
    || bad "regression: base-test FAILED (rc=$rc)"

  printf '\n=== HARVEST TEST 10b: m3-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m3-test passes" \
    || bad "regression: m3-test FAILED (rc=$rc)"

  printf '\n=== HARVEST TEST 10c: m4-test regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m4-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m4-test passes" \
    || bad "regression: m4-test FAILED (rc=$rc)"

  printf '\n=== HARVEST TEST 10d: b1-test regression ===\n'
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
  test_filter_noise_touches
  test_filter_exclude_override
  test_coder_brief_directive
  test_iter_cap_comment_accuracy
  test_tasks_subset_scopes_declared_snapshot
  test_regression

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
