#!/usr/bin/env bash
#
# bench-matrix-slice-brief-test.sh — tests for slice-scoped coder briefs
# (plan 699, task 4366). NO real claude invocations, NO network, NO spend.
#
# Test plan:
#   T1: Single-task strict slice — coder_brief includes that task's title+body
#       and the "implement ONLY" instruction.
#   T2: Two-task grouped slice — coder_brief includes BOTH tasks' titles+bodies
#       and the "implement ONLY" instruction.
#   T3: Task body containing markdown, quotes, and newlines does NOT break the
#       heredoc (brief still renders, no syntax error, body content present).
#   T4: No regression — all existing test suites still pass (harvest, resume,
#       m3, m4, b1, base, agent-flags).
#
# ISOLATION: each test uses a fresh temp PLANAR_DB + config so the real
# ~/.planar is NEVER named.
#
# Usage: scripts/bench-matrix-slice-brief-test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MATRIX="$SCRIPT_DIR/bench-matrix.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLANAR_BIN="${PLANAR_BIN:-planar}"

# Planning verbs refuse from inside a git worktree. Derive the primary checkout.
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
  git -C "$repo" config user.email slicebrief@test.local
  git -C "$repo" config user.name slice-brief-test
  printf 'base\n' >"$repo/seed.txt"
  git -C "$repo" add -A
  git -C "$repo" commit -qm base
  printf '%s' "$repo"
}

# seed_plan <db> <cfg> <corpus> — create a global-scope plan; prints plan id.
seed_plan() {
  local db="$1" cfg="$2" corpus="$3"
  ( cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
      command "$PLANAR_BIN" plan create "slice-brief fixture" --scope global --json 2>/dev/null \
    | jq -r '.id' )
}

# seed_task <db> <cfg> <corpus> <plan_id> <title> <body> — add one task with a
# body to <plan>; prints task id. The body is set via a separate SQLite update
# since the task add CLI may not support --body. Body is stored using SQLite
# single-quote escaping (replacing ' with ''), which handles newlines, backtick
# code spans, dollar signs, and embedded quotes safely without jq JSON encoding
# (which would produce \"…\" sequences that sqlite3 re-interprets as syntax).
seed_task() {
  local db="$1" cfg="$2" corpus="$3" plan="$4" title="$5" body="$6"
  local tid body_escaped
  tid="$(cd "$corpus" && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$cfg" \
    command "$PLANAR_BIN" task add "$title" --plan "$plan" --scope global \
    --json 2>/dev/null | jq -r '.id')"
  # Escape single-quotes for SQLite string literal: ' -> ''
  body_escaped="$(printf '%s' "$body" | sed "s/'/''/g")"
  sqlite3 "$db" "UPDATE tasks SET body = '${body_escaped}' WHERE id = ${tid};" 2>/dev/null || true
  printf '%s' "$tid"
}

# run_coder_brief_with_tasks <home> <db> <cfg> <corpus> <plan_id> <task_ids_json>
# — run coder_brief via a wrapper script that sets required env vars, stubs
# pl_plan_json to use the isolated DB, then prints the brief to stdout.
run_coder_brief_with_tasks() {
  local home="$1" db="$2" cfg="$3" corpus="$4" plan_id="$5" task_ids_json="$6"
  local trimmed="$home/matrix-nomain.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"
  local wrapper="$home/brief-wrapper-$$.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
# Override PLANNING_CWD to point at our isolated corpus (a primary checkout).
PLANNING_CWD="$corpus"
CELL_PROBLEM="stub plan problem statement"
coder_brief "$plan_id" "\$CELL_PROBLEM" '$task_ids_json'
WRAP
  chmod +x "$wrapper"
  bash "$wrapper"
}

# ===========================================================================
# TEST 1 — Single-task strict slice includes that task's title+body and
#           the "implement ONLY" instruction.
# ===========================================================================
test_single_task_brief() {
  printf '\n=== SLICE-BRIEF TEST 1: single-task slice includes title+body + ONLY instruction ===\n'
  local home db cfg corpus pid tid brief_out
  home="$(mktemp -d "${TMPDIR:-/tmp}/slice-brief-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  tid="$(seed_task "$db" "$cfg" "$corpus" "$pid" \
    "M7 follow-up: lift policies parsing into manifest.zig" \
    "Lift the policies.* parsing into manifest.zig (typed PolicyBundle). Acceptance: zig build clean, zig fmt clean, tests green.")"

  brief_out="$(run_coder_brief_with_tasks "$home" "$db" "$cfg" "$corpus" "$pid" "[${tid}]")"

  # The brief must contain the task title.
  if printf '%s\n' "$brief_out" | grep -qF "M7 follow-up: lift policies parsing into manifest.zig"; then
    ok "T1a: single-task brief contains the task title"
  else
    bad "T1a: brief does NOT contain the task title; brief excerpt: $(printf '%s\n' "$brief_out" | head -20)"
  fi

  # The brief must contain part of the task body.
  if printf '%s\n' "$brief_out" | grep -qF "typed PolicyBundle"; then
    ok "T1b: single-task brief contains the task body text"
  else
    bad "T1b: brief does NOT contain task body text; brief excerpt: $(printf '%s\n' "$brief_out" | head -20)"
  fi

  # The brief must contain the task ID.
  if printf '%s\n' "$brief_out" | grep -qF "Task ${tid}:"; then
    ok "T1c: brief contains 'Task <id>:' header for the task"
  else
    bad "T1c: brief does NOT contain 'Task ${tid}:' header"
  fi

  # The brief must contain the "implement ONLY" instruction.
  if printf '%s\n' "$brief_out" | grep -qi "implement ONLY"; then
    ok "T1d: brief contains 'implement ONLY' instruction"
  else
    bad "T1d: brief does NOT contain 'implement ONLY' instruction; brief excerpt: $(printf '%s\n' "$brief_out" | head -25)"
  fi

  # The brief must contain the "TASKS TO IMPLEMENT" section header.
  if printf '%s\n' "$brief_out" | grep -qF "TASKS TO IMPLEMENT"; then
    ok "T1e: brief contains 'TASKS TO IMPLEMENT' section header"
  else
    bad "T1e: brief does NOT contain 'TASKS TO IMPLEMENT' section header"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 2 — Two-task grouped slice includes BOTH tasks' titles+bodies and the
#           "implement ONLY" instruction.
# ===========================================================================
test_two_task_brief() {
  printf '\n=== SLICE-BRIEF TEST 2: two-task grouped slice includes BOTH task details ===\n'
  local home db cfg corpus pid tid1 tid2 brief_out
  home="$(mktemp -d "${TMPDIR:-/tmp}/slice-brief-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  tid1="$(seed_task "$db" "$cfg" "$corpus" "$pid" \
    "Add typed PolicyBundle struct" \
    "Define PolicyBundle in src/engine/planning/manifest.zig. Fields: policies []Policy.")"
  tid2="$(seed_task "$db" "$cfg" "$corpus" "$pid" \
    "Wire PolicyBundle into plan ingest path" \
    "Update ingest.zig to use PolicyBundle instead of raw JSON. Acceptance: zig build clean.")"

  brief_out="$(run_coder_brief_with_tasks "$home" "$db" "$cfg" "$corpus" "$pid" "[${tid1},${tid2}]")"

  # Brief must contain BOTH task titles.
  if printf '%s\n' "$brief_out" | grep -qF "Add typed PolicyBundle struct"; then
    ok "T2a: two-task brief contains first task title"
  else
    bad "T2a: brief does NOT contain first task title"
  fi

  if printf '%s\n' "$brief_out" | grep -qF "Wire PolicyBundle into plan ingest path"; then
    ok "T2b: two-task brief contains second task title"
  else
    bad "T2b: brief does NOT contain second task title"
  fi

  # Brief must contain body text from BOTH tasks.
  if printf '%s\n' "$brief_out" | grep -qF "Fields: policies []Policy"; then
    ok "T2c: two-task brief contains first task body text"
  else
    bad "T2c: brief does NOT contain first task body text"
  fi

  if printf '%s\n' "$brief_out" | grep -qF "Wire PolicyBundle into plan ingest path" && \
     printf '%s\n' "$brief_out" | grep -qF "ingest.zig"; then
    ok "T2d: two-task brief contains second task body text"
  else
    bad "T2d: brief does NOT contain second task body text"
  fi

  # Both task ID headers must appear.
  if printf '%s\n' "$brief_out" | grep -qF "Task ${tid1}:"; then
    ok "T2e: brief contains 'Task ${tid1}:' header"
  else
    bad "T2e: brief missing 'Task ${tid1}:' header"
  fi

  if printf '%s\n' "$brief_out" | grep -qF "Task ${tid2}:"; then
    ok "T2f: brief contains 'Task ${tid2}:' header"
  else
    bad "T2f: brief missing 'Task ${tid2}:' header"
  fi

  # "implement ONLY" must be present.
  if printf '%s\n' "$brief_out" | grep -qi "implement ONLY"; then
    ok "T2g: two-task brief contains 'implement ONLY' instruction"
  else
    bad "T2g: two-task brief does NOT contain 'implement ONLY' instruction"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 3 — Task body with markdown, quotes, newlines does NOT break the
#           heredoc — brief renders cleanly and contains the body text.
# ===========================================================================
test_body_with_special_chars() {
  printf '\n=== SLICE-BRIEF TEST 3: task body with markdown/quotes/newlines safe in heredoc ===\n'
  local home db cfg corpus pid tid brief_out
  home="$(mktemp -d "${TMPDIR:-/tmp}/slice-brief-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"

  # Body contains markdown heading, backtick code span, double quotes, single
  # quotes, dollar signs, and embedded newlines — the characters most likely to
  # break a naive heredoc expansion.
  local tricky_body
  tricky_body='## Acceptance criteria

- `zig build` passes with no warnings.
- All unit tests in `src/engine/` are green.
- The "typed" API surface exports `PolicyBundle` as a `pub const`.
- Cost: $0 extra allocations (use the plan'\''s arena).
- See decision D-17 for the rationale.'

  tid="$(seed_task "$db" "$cfg" "$corpus" "$pid" \
    "Task with tricky body chars" \
    "$tricky_body")"

  # Run coder_brief via the wrapper; assert it exits 0 and contains key body text.
  local wrapper_rc=0
  brief_out="$(run_coder_brief_with_tasks "$home" "$db" "$cfg" "$corpus" "$pid" "[${tid}]")" \
    || wrapper_rc=$?

  [ "$wrapper_rc" -eq 0 ] \
    && ok "T3a: coder_brief wrapper exited 0 (no syntax error from special chars in body)" \
    || bad "T3a: coder_brief wrapper FAILED (rc=${wrapper_rc}) — heredoc likely broken"

  if printf '%s\n' "$brief_out" | grep -qF "Acceptance criteria"; then
    ok "T3b: brief contains markdown heading from task body (## Acceptance criteria)"
  else
    bad "T3b: brief does NOT contain markdown heading from task body"
  fi

  if printf '%s\n' "$brief_out" | grep -qF 'PolicyBundle'; then
    ok "T3c: brief contains backtick-quoted identifier from task body"
  else
    bad "T3c: brief does NOT contain backtick content from task body"
  fi

  if printf '%s\n' "$brief_out" | grep -qF "D-17"; then
    ok "T3d: brief contains rest of body text (D-17 reference)"
  else
    bad "T3d: brief does NOT contain full body text"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 4 — Anti-sprawl: plan-problem sentinel is ABSENT; anti-sprawl language
#           and per-task content are PRESENT.
# ===========================================================================
#
# T4a: The full plan-level problem statement is NOT injected into the brief.
#      Seed a unique sentinel string as the problem; assert it is absent from
#      the rendered brief (the primary sprawl driver must be suppressed).
# T4b: Anti-sprawl constraint language IS present ("minimum set of files",
#      "out of scope", "Do NOT implement other tasks").
# T4c: The gate reframing IS present ("UNRELATED to your task" / "do NOT repair
#      unrelated code").
# T4d: Single-task and two-task shapes both pass T4a/T4b (no regression on
#      existing T1/T2 content assertions either).
# ===========================================================================
test_antisprawl_brief() {
  printf '\n=== SLICE-BRIEF TEST 4: anti-sprawl — sentinel absent, constraint language present ===\n'
  local home db cfg corpus pid tid brief_out

  home="$(mktemp -d "${TMPDIR:-/tmp}/slice-brief-test.XXXXXX")"
  db="$home/exp.db"; cfg="$home/config.toml"
  corpus="$(new_corpus "$home")"
  pid="$(seed_plan "$db" "$cfg" "$corpus")"
  tid="$(seed_task "$db" "$cfg" "$corpus" "$pid" \
    "Add migration for observed_settings table" \
    "Create migrations/00020_observed_settings.up.sql and .down.sql. No application code changes.")"

  # The problem string contains a unique sentinel that MUST NOT appear in the brief.
  local sentinel="SENTINEL_PLAN_PROBLEM_XK7Q9R"
  local problem_str="This plan adds full observability for settings changes. ${sentinel} Multi-milestone epic."

  # Render the brief via a wrapper that passes the sentinel as the problem arg.
  local trimmed="$home/matrix-nomain-antisprawl.sh"
  sed '$ s/^main "$@"$//' "$MATRIX" >"$trimmed"
  local wrapper="$home/antisprawl-wrapper-$$.sh"
  cat >"$wrapper" <<WRAP
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
PLANNING_CWD="$corpus"
coder_brief "$pid" "$problem_str" "[${tid}]"
WRAP
  chmod +x "$wrapper"
  brief_out="$(bash "$wrapper")"

  # T4a: sentinel must be ABSENT (plan-problem dump suppressed).
  if printf '%s\n' "$brief_out" | grep -qF "$sentinel"; then
    bad "T4a: brief CONTAINS the plan-problem sentinel '${sentinel}' — plan-problem dump not suppressed"
  else
    ok "T4a: plan-problem sentinel is ABSENT from brief (plan-problem dump suppressed)"
  fi

  # T4b: anti-sprawl constraint language must be PRESENT.
  if printf '%s\n' "$brief_out" | grep -qi "minimum set of files"; then
    ok "T4b: brief contains 'minimum set of files' anti-sprawl constraint"
  else
    bad "T4b: brief does NOT contain 'minimum set of files' constraint"
  fi

  if printf '%s\n' "$brief_out" | grep -qF "out of scope"; then
    ok "T4c: brief contains 'out of scope' anti-sprawl warning"
  else
    bad "T4c: brief does NOT contain 'out of scope' warning"
  fi

  if printf '%s\n' "$brief_out" | grep -qF "Do NOT implement other tasks"; then
    ok "T4d: brief contains 'Do NOT implement other tasks' constraint"
  else
    bad "T4d: brief does NOT contain 'Do NOT implement other tasks' constraint"
  fi

  # T4e: gate reframing must be present.
  if printf '%s\n' "$brief_out" | grep -qF "do NOT repair unrelated code"; then
    ok "T4e: brief contains 'do NOT repair unrelated code' gate reframing"
  else
    bad "T4e: brief does NOT contain gate reframing text"
  fi

  # T4f: the per-task title+body is still present (not suppressed along with problem).
  if printf '%s\n' "$brief_out" | grep -qF "Add migration for observed_settings table"; then
    ok "T4f: brief still contains the task title (task content not accidentally stripped)"
  else
    bad "T4f: brief does NOT contain task title — task content was stripped"
  fi

  # T4g: two-task shape — sentinel still absent when two tasks are sliced together.
  local tid2
  tid2="$(seed_task "$db" "$cfg" "$corpus" "$pid" \
    "Wire observed_settings into the config reload path" \
    "Update src/engine/config.zig to read from observed_settings on reload.")"
  local wrapper2="$home/antisprawl-wrapper2-$$.sh"
  cat >"$wrapper2" <<WRAP2
#!/usr/bin/env bash
set -euo pipefail
export PLANAR_DB_OVERRIDE="$db"
export PLANAR_CONFIG_PATH_OVERRIDE="$cfg"
export BENCH_HOME="$home"
export BENCH_CORPUS_REPO="$corpus"
# shellcheck disable=SC1090
source "$trimmed"
PLANNING_CWD="$corpus"
coder_brief "$pid" "$problem_str" "[${tid},${tid2}]"
WRAP2
  chmod +x "$wrapper2"
  local brief_out2
  brief_out2="$(bash "$wrapper2")"

  if printf '%s\n' "$brief_out2" | grep -qF "$sentinel"; then
    bad "T4g: two-task brief CONTAINS the plan-problem sentinel — dump not suppressed in two-task shape"
  else
    ok "T4g: sentinel absent from two-task brief (plan-problem dump suppressed in two-task shape)"
  fi

  if printf '%s\n' "$brief_out2" | grep -qF "Wire observed_settings into the config reload path"; then
    ok "T4h: two-task brief contains second task title"
  else
    bad "T4h: two-task brief does NOT contain second task title"
  fi

  rm -rf "$home"
}

# ===========================================================================
# TEST 5 — No regression: all existing suites still pass.
# ===========================================================================
test_regression() {
  local rc
  printf '\n=== SLICE-BRIEF TEST 5a: harvest regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-harvest-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: harvest-test passes" \
    || bad "regression: harvest-test FAILED (rc=$rc)"

  printf '\n=== SLICE-BRIEF TEST 5b: resume regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-resume-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: resume-test passes" \
    || bad "regression: resume-test FAILED (rc=$rc)"

  printf '\n=== SLICE-BRIEF TEST 5c: m3 regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m3-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m3-test passes" \
    || bad "regression: m3-test FAILED (rc=$rc)"

  printf '\n=== SLICE-BRIEF TEST 5d: m4 regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-m4-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: m4-test passes" \
    || bad "regression: m4-test FAILED (rc=$rc)"

  printf '\n=== SLICE-BRIEF TEST 5e: b1 regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-b1-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: b1-test passes" \
    || bad "regression: b1-test FAILED (rc=$rc)"

  printf '\n=== SLICE-BRIEF TEST 5f: base regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-base-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: base-test passes" \
    || bad "regression: base-test FAILED (rc=$rc)"

  printf '\n=== SLICE-BRIEF TEST 5g: agent-flags regression ===\n'
  rc=0; bash "$SCRIPT_DIR/bench-matrix-agent-flags-test.sh" || rc=$?
  [ "$rc" -eq 0 ] \
    && ok "regression: agent-flags-test passes" \
    || bad "regression: agent-flags-test FAILED (rc=$rc)"
}

# ===========================================================================
# main
# ===========================================================================
main() {
  command -v jq      >/dev/null || { printf 'jq required\n'; exit 2; }
  command -v sqlite3 >/dev/null || { printf 'sqlite3 required\n'; exit 2; }
  command -v git     >/dev/null || { printf 'git required\n'; exit 2; }

  test_single_task_brief
  test_two_task_brief
  test_body_with_special_chars
  test_antisprawl_brief
  test_regression

  printf '\n=== RESULTS: %d passed, %d failed ===\n' "$pass" "$fail"
  [ "$fail" -eq 0 ]
}

main "$@"
