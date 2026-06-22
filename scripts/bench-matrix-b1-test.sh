#!/usr/bin/env bash
#
# bench-matrix-b1-test.sh — focused test for the B1 data-corruption fix:
# harvest_slice and fanin_conflict_check must AGREE on the slice's COMMITTED
# state. NO agent spawns — a stub commits known files into slice worktrees, then
# we drive harvest_slice + fanin_conflict_check directly (the functions are in
# scope after sourcing the matrix with `main` stripped).
#
# Proves:
#   1. A committed file (INCLUDING a created/untracked-then-committed file) is
#      recorded by harvest_slice as a kind='actual' touch via the committed
#      range (base..committed-HEAD), and fanin_conflict_check sees the same
#      committed HEAD and integrates it cleanly (no conflict event).
#   2. Overlapping COMMITTED edits across two slices produce a conflict event.
#   3. A slice that commits NOTHING yields zero actual touches AND a clean
#      fan-in no-op (no git call in the corpus repo) — the consistent
#      "task touched nothing" outcome, not a mis-record.
#
# ISOLATION: a fresh temp PLANAR_DB + a throwaway corpus git repo per test.
# The real ~/.planar is NEVER named.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
PLANAR_BIN="${PLANAR_BIN:-planar}"

# Each test runs in a SUBSHELL (to isolate the sourced matrix env + globals), so
# ok/bad cannot mutate the parent's counters. Each test subshell EXITS with its
# own failure count; the parent (main) sums those exit codes. ok/bad print and
# track a subshell-local _fail.
_fail=0
ok()  { printf 'PASS: %s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*"; _fail=$((_fail + 1)); }
total_fail=0

# new_corpus <home> — init a throwaway git repo with one base commit, print its
# path and leave $BASE_SHA set (caller reads it).
new_corpus() {
  local home="$1" repo="$1/corpus"
  mkdir -p "$repo"
  git -C "$repo" init -q
  git -C "$repo" config user.email b1@test.local
  git -C "$repo" config user.name b1-test
  printf 'base\n' >"$repo/seed.txt"
  git -C "$repo" add -A
  git -C "$repo" commit -qm base
  printf '%s' "$repo"
}

# seed_plan <db> <cfg> <corpus> — create a global-scope plan in the isolated DB
# so `bench start --plan <id>` satisfies the runs.plan_id FK. The corpus is a
# fresh PRIMARY checkout (not a worktree), so the planning verb runs there
# without tripping the worktree planning-verb split. Prints the plan id.
seed_plan() {
  local db="$1" cfg="$2" corpus="$3"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" plan create "b1 fixture" --scope global --json 2>/dev/null \
    | jq -r '.id' )
}

# commit_into <worktree> <relpath> <content> — write+commit a file in a worktree
# (simulates a coder that commits its work, per the fixed coder brief).
commit_into() {
  local wt="$1" rel="$2" content="$3"
  printf '%s\n' "$content" >"$wt/$rel"
  git -C "$wt" add -A
  git -C "$wt" commit -qm "edit $rel"
}

# source_matrix <db> <cfg> <corpus> — source the matrix (main stripped) so its
# functions + globals are in scope, pointed at the isolated DB and corpus.
# Returns by populating the caller's environment (run inside the test shell).
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

# ---------------------------------------------------------------------------
# TEST 1 — committed work (incl a created file): harvest records it as 'actual'
# via the committed range, AND fan-in integrates it cleanly (no conflict).
# ---------------------------------------------------------------------------
test_committed_agrees() {(
  local home db cfg corpus base wt integ pid
  home="$(mktemp -d "${TMPDIR:-/tmp}/b1test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"

  source_matrix "$db" "$cfg" "$corpus"
  CELL_RUN_UID="b1-committed"
  CELL_BASE_SHA="$base"
  CELL_ARM="strict"
  CELL_INTEG_BRANCH="bench-integ/$CELL_RUN_UID"

  pl bench start "$CELL_RUN_UID" --plan "$pid" --arm strict \
    --base-sha "$base" --config-hash b1 >/dev/null 2>&1

  # Integration worktree off base (the conflict-merge target).
  integ="$home/integ"
  git -C "$corpus" worktree add -q --detach "$integ" "$base"
  git -C "$integ" checkout -q -b "$CELL_INTEG_BRANCH"
  INTEG_WORKTREE="$integ"

  # Slice worktree: a coder that COMMITS a created file (NEW path, not in base).
  wt="$home/slice1"
  git -C "$corpus" worktree add -q --detach "$wt" "$base"
  commit_into "$wt" "created_by_coder.txt" "hello"

  # Drive the two instruments against the committed slice.
  fanin_conflict_check "$wt" '[1]'
  harvest_slice "$wt" '[1]'

  # Harvest must have recorded the CREATED file as kind='actual'.
  local n_actual has_created
  n_actual="$(sqlite3 "$db" \
    "select count(*) from run_touches t join runs r on r.id=t.run_id
     where r.run_uid='$CELL_RUN_UID' and t.kind='actual';")"
  has_created="$(sqlite3 "$db" \
    "select count(*) from run_touches t join runs r on r.id=t.run_id
     where r.run_uid='$CELL_RUN_UID' and t.kind='actual'
       and t.path='created_by_coder.txt';")"
  [ "$n_actual" -ge 1 ] \
    && ok "committed: harvest recorded >=1 actual touch (got $n_actual)" \
    || bad "committed: harvest recorded $n_actual actual touches (expected >=1)"
  [ "$has_created" -eq 1 ] \
    && ok "committed: created-then-committed file recorded via range mode" \
    || bad "committed: created file NOT recorded (range mode missed it)"

  # Fan-in saw the committed HEAD and integrated cleanly: NO conflict event.
  local n_conflict
  n_conflict="$(sqlite3 "$db" \
    "select count(*) from run_events e join runs r on r.id=e.run_id
     where r.run_uid='$CELL_RUN_UID' and e.kind='conflict';")"
  [ "$n_conflict" -eq 0 ] \
    && ok "committed: clean fan-in, no conflict event (instruments agree)" \
    || bad "committed: spurious conflict event ($n_conflict)"

  # And the integration branch now carries the file (fan-in really merged it).
  [ -f "$integ/created_by_coder.txt" ] \
    && ok "committed: integration branch carries the merged committed file" \
    || bad "committed: integration branch missing the merged file"

  rm -rf "$home"
  exit "$_fail"
)}

# ---------------------------------------------------------------------------
# TEST 2 — overlapping COMMITTED edits across two slices -> conflict event.
# ---------------------------------------------------------------------------
test_conflict_on_overlap() {(
  local home db cfg corpus base integ wt1 wt2 pid
  home="$(mktemp -d "${TMPDIR:-/tmp}/b1test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"

  source_matrix "$db" "$cfg" "$corpus"
  CELL_RUN_UID="b1-conflict"
  CELL_BASE_SHA="$base"
  CELL_ARM="grouped"
  CELL_INTEG_BRANCH="bench-integ/$CELL_RUN_UID"

  pl bench start "$CELL_RUN_UID" --plan "$pid" --arm grouped \
    --base-sha "$base" --config-hash b1 >/dev/null 2>&1

  integ="$home/integ"
  git -C "$corpus" worktree add -q --detach "$integ" "$base"
  git -C "$integ" checkout -q -b "$CELL_INTEG_BRANCH"
  INTEG_WORKTREE="$integ"

  # Two slices commit CONFLICTING edits to the SAME existing line of seed.txt.
  wt1="$home/sliceA"; wt2="$home/sliceB"
  git -C "$corpus" worktree add -q --detach "$wt1" "$base"
  git -C "$corpus" worktree add -q --detach "$wt2" "$base"
  commit_into "$wt1" "seed.txt" "from-slice-A"
  commit_into "$wt2" "seed.txt" "from-slice-B"

  # First slice integrates cleanly; second conflicts on the same line.
  fanin_conflict_check "$wt1" '[10]'
  fanin_conflict_check "$wt2" '[20]'

  local n_conflict task20
  n_conflict="$(sqlite3 "$db" \
    "select count(*) from run_events e join runs r on r.id=e.run_id
     where r.run_uid='$CELL_RUN_UID' and e.kind='conflict';")"
  task20="$(sqlite3 "$db" \
    "select count(*) from run_events e join runs r on r.id=e.run_id
     where r.run_uid='$CELL_RUN_UID' and e.kind='conflict'
       and json_extract(e.payload,'\$.task_ids')='[20]';")"
  [ "$n_conflict" -eq 1 ] \
    && ok "conflict: overlapping committed edits emit exactly 1 conflict event" \
    || bad "conflict: expected 1 conflict event, got $n_conflict"
  [ "$task20" -eq 1 ] \
    && ok "conflict: event carries the conflicting slice's task ids ([20])" \
    || bad "conflict: event task_ids mismatch"

  # The aborted merge left the integration branch usable (first slice intact).
  git -C "$integ" status --porcelain | grep -q '^UU' \
    && bad "conflict: integration branch left mid-merge (abort failed)" \
    || ok "conflict: half-merge aborted, integration branch clean"

  rm -rf "$home"
  exit "$_fail"
)}

# ---------------------------------------------------------------------------
# TEST 3 — no-commit slice: zero actual touches AND a clean fan-in no-op with
# NO git call in the corpus repo (C1). We assert the corpus working tree is
# untouched by the no-op and no conflict/actual rows are written.
# ---------------------------------------------------------------------------
test_no_commit_consistent() {(
  local home db cfg corpus base integ wt corpus_status_before corpus_status_after pid
  home="$(mktemp -d "${TMPDIR:-/tmp}/b1test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  base="$(git -C "$corpus" rev-parse HEAD)"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"

  source_matrix "$db" "$cfg" "$corpus"
  CELL_RUN_UID="b1-nocommit"
  CELL_BASE_SHA="$base"
  CELL_ARM="strict"
  CELL_INTEG_BRANCH="bench-integ/$CELL_RUN_UID"

  pl bench start "$CELL_RUN_UID" --plan "$pid" --arm strict \
    --base-sha "$base" --config-hash b1 >/dev/null 2>&1

  integ="$home/integ"
  git -C "$corpus" worktree add -q --detach "$integ" "$base"
  git -C "$integ" checkout -q -b "$CELL_INTEG_BRANCH"
  INTEG_WORKTREE="$integ"

  # Slice worktree that commits NOTHING (HEAD == base).
  wt="$home/slice0"
  git -C "$corpus" worktree add -q --detach "$wt" "$base"

  corpus_status_before="$(git -C "$corpus" rev-parse HEAD):$(git -C "$corpus" status --porcelain | wc -l | tr -d ' ')"
  fanin_conflict_check "$wt" '[99]'
  harvest_slice "$wt" '[99]'
  corpus_status_after="$(git -C "$corpus" rev-parse HEAD):$(git -C "$corpus" status --porcelain | wc -l | tr -d ' ')"

  # C1: the corpus repo's own HEAD + working tree are untouched (no merge ran).
  [ "$corpus_status_before" = "$corpus_status_after" ] \
    && ok "no-commit: corpus repo untouched by fan-in no-op (C1: no git merge)" \
    || bad "no-commit: corpus repo MUTATED ($corpus_status_before -> $corpus_status_after)"

  # Zero actual touches and zero conflict events — the consistent outcome.
  local n_actual n_conflict
  n_actual="$(sqlite3 "$db" \
    "select count(*) from run_touches t join runs r on r.id=t.run_id
     where r.run_uid='$CELL_RUN_UID' and t.kind='actual';")"
  n_conflict="$(sqlite3 "$db" \
    "select count(*) from run_events e join runs r on r.id=e.run_id
     where r.run_uid='$CELL_RUN_UID' and e.kind='conflict';")"
  [ "$n_actual" -eq 0 ] \
    && ok "no-commit: harvest recorded zero actual touches (correct)" \
    || bad "no-commit: harvest recorded $n_actual actual touches (expected 0)"
  [ "$n_conflict" -eq 0 ] \
    && ok "no-commit: clean fan-in no-op, zero conflict events" \
    || bad "no-commit: $n_conflict conflict events (expected 0)"

  rm -rf "$home"
  exit "$_fail"
)}

main() {
  command -v jq >/dev/null      || { echo "jq required"; exit 2; }
  command -v sqlite3 >/dev/null || { echo "sqlite3 required"; exit 2; }
  command -v git >/dev/null     || { echo "git required"; exit 2; }
  local rc
  printf '=== B1 TEST 1: committed work (incl created file) — instruments agree ===\n'
  rc=0; test_committed_agrees || rc=$?; total_fail=$((total_fail + rc))
  printf '\n=== B1 TEST 2: overlapping committed edits — conflict event ===\n'
  rc=0; test_conflict_on_overlap || rc=$?; total_fail=$((total_fail + rc))
  printf '\n=== B1 TEST 3 (C1): no-commit slice — zero actual + clean no-op ===\n'
  rc=0; test_no_commit_consistent || rc=$?; total_fail=$((total_fail + rc))
  printf '\n=== RESULTS: %d assertion(s) failed ===\n' "$total_fail"
  [ "$total_fail" -eq 0 ]
}

main "$@"
