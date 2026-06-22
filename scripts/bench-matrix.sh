#!/usr/bin/env bash
#
# bench-matrix.sh — the confirmatory-run matrix driver for the
# closure-measurement experiment (plan 699, M1: single-cell foundation).
#
# This is the D-HARNESS "confirmatory driver": a bash script (deliberately
# NOT a planar binary, so the D5 boundary stays self-evident — planar-execute
# never spawns models; the B-phase spawning is the harness's job). It drives
# ONE experiment cell `(plan, arm=strict, rep=1)` end-to-end:
#
#   pick base_sha (+ verify the feature is absent at base)
#     -> create a cycle worktree off base_sha
#     -> planar-execute run bench_run_ritual.lua --phase setup   (A: reset + bench start)
#     -> emit slice_dispatch event
#     -> spawn_agent coder    (B: headless `claude -p --output-format=json`)
#     -> spawn_agent reviewer (B: verdict gates, iteration cap 5)
#     -> emit token_sample per agent
#     -> emit slice_fanin event
#     -> planar-execute run bench_run_ritual.lua --phase measure  (C: harvest + finish)
#
# SCOPE (M2): the three arm shapes + fan-in conflict detection, on top of
# M1's telemetry capture + base-SHA gate + --dry-run. One cell = one
# (plan, arm, rep); the cell opens ONE run record, then dispatches its tasks
# in the arm's characteristic shape:
#   * strict      — one isolated worktree per task, serial; one
#                   slice_dispatch/slice_fanin per task; M-BLAST=1.
#   * eligibility — `plan recommend-strategy`: the parallel-ELIGIBLE subset
#                   runs CONCURRENTLY (one worktree each off base_sha, agents
#                   as background jobs, `wait` = the fan-in barrier); the
#                   SERIALIZED remainder runs serially after. One
#                   slice_dispatch/slice_fanin per task.
#   * grouped     — `groups recommend --solver <S>`: each returned slice is a
#                   co-located task set in ONE shared worktree worked by one
#                   coder; one slice_dispatch/slice_fanin per SLICE; harvest is
#                   per-task-in-slice; M-BLAST = slice size.
# At every slice/cohort fan-in the harness attempts to merge the committed work
# onto a per-arm integration branch off base_sha; a merge failure emits a
# `conflict` event (M-CONF). The arm shapes share one `run_slice` worker.
#
# Still deliberately NOT implemented (M3): the matrix loop, arm interleaving
# per (rep,plan), N reps, paired config_hash sweeps, spend-ceiling stop, and
# resume-via-run_uid. The code is structured (cell-level setup/finish brackets
# a `dispatch_<arm>` call) so M3 wraps run_cell in an outer loop.
#
# ISOLATION INVARIANT: every `planar`, `planar-execute`, and agent subprocess
# runs against an ISOLATED experiment PLANAR_DB + PLANAR_CONFIG_PATH. The real
# ~/.planar/planar.db is NEVER touched. The env is exported once at the top so
# every child (including ones the Lua ritual shells) inherits it.
#
# CONVENTIONS (match scripts/parity-audit.sh): pure bash 3.2 (no associative
# arrays, no mapfile), set -euo pipefail, requires jq, fully unattended,
# read-only against the live DB by virtue of never naming it.
#
# Usage
#   scripts/bench-matrix.sh --plan <id> [--arm strict|eligibility|grouped] [--rep 1] [--dry-run]
#   scripts/bench-matrix.sh --plan <id> --arm grouped --solver mtkahypar --dry-run
#   scripts/bench-matrix.sh --plan <id> --tasks 101,102 --dry-run
#   scripts/bench-matrix.sh --help

set -euo pipefail

# ===========================================================================
# 1. CONFIG + ISOLATION  (top-of-file, operator-editable)
# ===========================================================================

# --- model routing (held constant across arms; frozen into config_hash) ---
CODER_MODEL="${BENCH_CODER_MODEL:-claude-sonnet-4-5}"
REVIEWER_MODEL="${BENCH_REVIEWER_MODEL:-claude-sonnet-4-5}"

# --- iteration cap (preregistration §3: the objective gate may need retries) ---
ITER_CAP="${BENCH_ITER_CAP:-5}"

# --- the brief-template version, part of config_hash (frozen nuisance var) ---
BRIEF_TEMPLATE_VERSION="m1.v1"

# --- grouped-arm slicer solver. `planar groups recommend --solver <S>` returns
#     the co-located task slices. greedy is the always-available default; the
#     mtkahypar partitioner is optional (used iff the binary was built with it).
#     Held constant within a config_hash group (it shapes only the grouped arm). ---
SOLVER="${BENCH_SOLVER:-greedy}"

# --- the corpus repo under measurement. Defaults to THIS planar checkout
#     (the experiment measures planar's own decomposition). Override for a
#     different corpus member. The cycle worktree is cut from here. ---
CORPUS_REPO_PATH="${BENCH_CORPUS_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CORPUS_REPO_NAME="${BENCH_CORPUS_REPO_NAME:-planar}"

# --- PLANNING_CWD: where the cwd-derived PLANNING-verb reads run from.
#     CORPUS_REPO_PATH is frequently a git WORKTREE (the harness itself lives in
#     one), and planar refuses planning verbs (`groups recommend`,
#     `plan recommend-strategy`, plan/task reads behind the scope guard) from
#     inside a worktree — "worktrees are for code execution, not planning the
#     work itself" (the worktree planning-verb split). So all read-only planning
#     queries are issued from the corpus repo's PRIMARY (non-worktree) checkout,
#     which `git worktree list --porcelain` always lists first. Override with
#     BENCH_PLANNING_CWD for an unusual layout. ---
PLANNING_CWD="${BENCH_PLANNING_CWD:-$(git -C "$CORPUS_REPO_PATH" worktree list --porcelain 2>/dev/null \
  | awk '/^worktree /{print $2; exit}')}"
[ -n "$PLANNING_CWD" ] || PLANNING_CWD="$CORPUS_REPO_PATH"

# --- ISOLATED experiment plane. NEVER ~/.planar. A temp DB + config dir,
#     created fresh per invocation unless the operator pins them (resume, M3).
#     These are exported below so EVERY subprocess inherits the isolation. ---
BENCH_HOME="${BENCH_HOME:-$(mktemp -d "${TMPDIR:-/tmp}/bench-matrix.XXXXXX")}"
export PLANAR_DB="${PLANAR_DB_OVERRIDE:-$BENCH_HOME/experiment.db}"
export PLANAR_CONFIG_PATH="${PLANAR_CONFIG_PATH_OVERRIDE:-$BENCH_HOME/config.toml}"

# --- where cycle worktrees + agent transcripts land (keyed by run_uid) ---
WORKTREE_ROOT="${BENCH_WORKTREE_ROOT:-$BENCH_HOME/worktrees}"
TRANSCRIPT_ROOT="${BENCH_TRANSCRIPT_ROOT:-$BENCH_HOME/transcripts}"

# --- the planar binaries (PATH-resolved; pin via env for a fresh local build) ---
PLANAR_BIN="${PLANAR_BIN:-planar}"
PLANAR_EXECUTE_BIN="${PLANAR_EXECUTE_BIN:-planar-execute}"

# --- the deterministic A/C ritual the harness brackets the B-phase with ---
RITUAL_LUA="${BENCH_RITUAL_LUA:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/workflows/bench_run_ritual.lua}"

# --- runtime state (parsed from argv) ---
ARG_PLAN=""
ARG_REP=1
ARG_TASKS=""        # optional comma-list; empty = all plan tasks
ARG_ARM="strict"    # strict | eligibility | grouped (M2: the three arm shapes)
DRY_RUN=0

# ===========================================================================
# helpers (match parity-audit.sh logging style)
# ===========================================================================

log()   { printf '  %s\n' "$*" >&2; }
title() { printf '\n==> %s\n' "$*" >&2; }
err()   { printf 'bench-matrix: %s\n' "$*" >&2; exit 1; }

# require_tool <name> — fail loudly if a hard dependency is missing.
require_tool() {
  command -v "$1" >/dev/null 2>&1 || err "required tool not found on PATH: $1"
}

# pl(...) — every `planar` invocation goes through here so the isolated DB env
# is guaranteed (it is exported globally, but this is the single seam M3 can
# wrap for tracing/spend accounting). Named `pl` (not `planar`) so it never
# shadows the binary: `command "$PLANAR_BIN"` bypasses function lookup and
# resolves the real executable on PATH.
pl() { command "$PLANAR_BIN" "$@"; }

# pl_json(...) — `planar` call whose stdout is JSON (validated by jq later).
pl_json() { command "$PLANAR_BIN" "$@"; }

# pl_plan_json(...) — a READ-ONLY planning query (`plan show`, `task list`,
# `recommend-strategy`, `groups recommend`, `decision list`, `task touches
# list`, `assoc list`). These are subject to the worktree planning-verb split:
# planar refuses them from inside a git worktree. They are issued from
# PLANNING_CWD (the corpus repo's primary checkout) so the cwd-derive guard is
# satisfied even when CORPUS_REPO_PATH is a worktree. Read-only by construction.
pl_plan_json() { ( cd "$PLANNING_CWD" && command "$PLANAR_BIN" "$@" ); }

# execute_phase <phase> <worktree> <args-json> — run one deterministic ritual
# phase via planar-execute. Returns the flow.result JSON on stdout.
execute_phase() {
  "$PLANAR_EXECUTE_BIN" run "$RITUAL_LUA" \
    --phase "$1" --worktree "$2" --args "$3"
}

# ===========================================================================
# 2. TELEMETRY CAPTURE  (M0-proven field mapping)
# ===========================================================================
#
# spawn_agent <model> <worktree> <brief>  ->  prints a token_sample JSON object
#
# Runs a headless agent and captures the TERMINAL session-cumulative usage
# object. M0 confirmed these fields exist on the result JSON:
#   in        = .usage.input_tokens                 (probe: 5486)
#   out       = .usage.output_tokens                (probe: 4)
#   cache_in  = .usage.cache_read_input_tokens      (probe: 15626)
#   cache_out = .usage.cache_creation_input_tokens  (probe: 2129)
#   usd       = .total_cost_usd                      (probe: $0.057)
#
# CRITICAL (D-HARNESS): the result `usage` is the SESSION-CUMULATIVE aggregate
# for the whole agent run — it is NOT a per-turn delta. We use it as the
# whole-agent total and MUST NOT sum across turns (summing double-counts the
# cache_read tokens, which the schema doc and decision 547 both call out).
#
# stdin is /dev/null so the headless agent never blocks waiting for input.
# The raw result JSON is archived under $TRANSCRIPT_ROOT keyed by run_uid+role
# so a recorded measurement survives independently of the parsed sample.
spawn_agent() {
  local model="$1" worktree="$2" brief="$3"
  local raw

  # Headless, non-interactive, single JSON result object. --add-dir grants the
  # agent the cycle worktree; cwd is the worktree so relative paths resolve.
  # --permission-mode acceptEdits lets the coder edit files unattended (the
  # worktree is an isolated, throwaway, internet-free sandbox — the documented
  # use case for skipping the interactive permission prompt).
  #
  # The brief is fed on STDIN (not as a positional). Empirically, claude
  # 2.1.x `--print` with a redirected stdin (`< /dev/null`) reads the prompt
  # from stdin and ignores any positional, erroring "Input must be provided
  # ... when using --print". Piping the brief in is the portable headless
  # shape; it also avoids argv-length limits on large briefs.
  raw="$(cd "$worktree" && printf '%s' "$brief" | claude -p \
    --output-format=json \
    --model "$model" \
    --permission-mode acceptEdits \
    --add-dir "$worktree")"

  # Map the M0-confirmed fields into the frozen token_sample payload shape.
  # Missing fields coerce to 0 (// 0) so a partial usage object never aborts
  # the cell — the absence is itself recorded (a 0 sample is a measurement).
  printf '%s' "$raw" | jq -c '{
    in:        (.usage.input_tokens                  // 0),
    out:       (.usage.output_tokens                 // 0),
    cache_in:  (.usage.cache_read_input_tokens       // 0),
    cache_out: (.usage.cache_creation_input_tokens   // 0),
    usd:       (.total_cost_usd                       // 0)
  }'
}

# emit_token_sample <run_uid> <seq> <role> <sample-json>
# Records one agent's whole-run usage as a bench token_sample event.
# `role` (coder|reviewer) rides in the payload so M-TOK can attribute spend.
emit_token_sample() {
  local run_uid="$1" seq="$2" role="$3" sample="$4"
  local payload
  payload="$(printf '%s' "$sample" | jq -c --arg role "$role" '. + {role:$role}')"
  pl bench event "$run_uid" --kind token_sample --seq "$seq" --payload "$payload"
}

# ===========================================================================
# 3. BASE-SHA SELECTION + VERIFICATION GATE  (D-HARNESS)
# ===========================================================================
#
# declared_paths <plan> — the plan's declared touch paths (the predicted
# closure), de-duplicated. Read from task_touch_paths via `task touches list`
# across the plan's tasks. This is the closure the strict arm dispatches and
# the set base-SHA fidelity is verified against.
declared_paths() {
  local plan="$1" scope task_ids tid
  scope="$(plan_scope "$plan")"
  task_ids="$(pl_plan_json task list --plan "$plan" --scope "$scope" --json \
                | jq -r '.[].id')"
  : "$scope"  # scope is used for the task-list read; touches list needs none
  for tid in $task_ids; do
    # `task touches list` is a read (no --scope flag); its JSON shape is
    # {task_id, repos:[...], paths:[{repo, path}]}. Pull .paths[].path.
    pl_plan_json task touches list "$tid" --json 2>/dev/null \
      | jq -r '.paths[]?.path // empty' 2>/dev/null || true
  done | sort -u
}

# plan_scope <plan> — resolve the --scope slug a plan's writes need (the
# cwd-derive guard refuses bare reads from outside a registered project).
# Derived from scope_kind/scope_id on `plan show`.
plan_scope() {
  local plan="$1" kind id slug
  local pj
  pj="$(pl_plan_json plan show "$plan" --json)"
  kind="$(printf '%s' "$pj" | jq -r '.scope_kind')"
  id="$(printf '%s' "$pj" | jq -r '.scope_id')"
  if [ "$kind" = "global" ]; then
    printf 'global'
    return 0
  fi
  # association scope: map scope_id -> slug as "project:<slug>" / "<slug>".
  slug="$(pl_plan_json assoc list --json | jq -r --argjson id "$id" \
            '.[] | select(.id==$id) | .slug')"
  [ -n "$slug" ] || err "plan_scope: could not resolve scope slug for plan $plan (scope_id=$id)"
  printf '%s' "$slug"
}

# first_touching_commit <repo> <path...> — the FIRST commit (oldest) in the
# repo whose change set touches any of the given paths. `git log --reverse`
# walks oldest-first; head -1 takes the earliest.
first_touching_commit() {
  local repo="$1"; shift
  git -C "$repo" log --reverse --format=%H -- "$@" 2>/dev/null | head -n 1
}

# pick_base_sha <repo> <path...> — base_sha = PARENT of the first commit in
# the plan's lineage that touches the declared paths (D-HARNESS). That parent
# is the last clean state before the feature was introduced. Printed on stdout.
pick_base_sha() {
  local repo="$1"; shift
  local first
  first="$(first_touching_commit "$repo" "$@")"
  [ -n "$first" ] || err "pick_base_sha: no commit in $repo touches the declared paths; cannot locate a clean base"
  # Parent of the first touching commit. If `first` is the root commit it has
  # no parent and there IS no clean base — refuse rather than guess.
  git -C "$repo" rev-parse --verify "${first}^" 2>/dev/null \
    || err "pick_base_sha: first touching commit $first is a root commit; no pre-feature base exists"
}

# verify_feature_absent <repo> <base_sha> <path...> — the BASE-FIDELITY GATE.
# Asserts the feature does NOT already exist at base_sha. The check: the
# declared paths must be ABSENT (or empty of the acceptance signal) at base —
# if the file the plan claims to create already exists at base, the cell is
# measuring a no-op and the base is wrong. Refuse the cell with a clear error.
#
# M1 uses path-existence as the absence signal (path-level harvest is the MVP,
# decision D1). A path that already exists at base is the pre-existence bug
# this gate fixes. Later milestones can tighten this to symbol/acceptance-
# signal absence; the seam is this function.
verify_feature_absent() {
  local repo="$1" base="$2"; shift 2
  local p preexisting=""
  for p in "$@"; do
    # `git cat-file -e <sha>:<path>` exits 0 iff the path exists in that tree.
    if git -C "$repo" cat-file -e "${base}:${p}" 2>/dev/null; then
      preexisting="${preexisting}${preexisting:+ }${p}"
    fi
  done
  if [ -n "$preexisting" ]; then
    err "base-fidelity gate FAILED: declared path(s) already exist at base_sha ${base}: ${preexisting}
       the feature pre-exists at base; this cell would measure a no-op. Refusing."
  fi
  log "base-fidelity gate OK: no declared path pre-exists at base ${base}"
}

# ===========================================================================
# run_uid minting + config_hash  (D8 / run-record-schema §2)
# ===========================================================================
#
# mint_run_uid <plan> <arm> <rep> — harness-minted stable id. Encodes the cell
# coordinates + a timestamp for human readability and uniqueness. (M3 mints
# -retryN suffixes on resume; never reuses a partial — out of scope here.)
mint_run_uid() {
  printf 'bench-%s-%s-rep%s-%s' "$1" "$2" "$3" "$(date -u +%Y%m%dT%H%M%SZ)"
}

# config_hash <arm> — the GROUP BY key. Hashes the frozen nuisance vars
# (models, iteration cap, brief-template version) PLUS the arm. Paired cells
# differ only in arm (D-HARNESS). sha256, first 16 hex chars.
config_hash() {
  local arm="$1"
  printf '%s|%s|%s|%s|%s' \
    "$CODER_MODEL" "$REVIEWER_MODEL" "$ITER_CAP" "$BRIEF_TEMPLATE_VERSION" "$arm" \
    | shasum -a 256 | awk '{print substr($1,1,16)}'
}

# config_json <arm> <base_sha> — the opaque audit blob config_hash is taken
# over, plus the resolved base_sha (stored in the run so the cell is
# reproducible from its own record).
config_json() {
  local arm="$1" base="$2"
  jq -nc \
    --arg coder "$CODER_MODEL" \
    --arg reviewer "$REVIEWER_MODEL" \
    --argjson iter_cap "$ITER_CAP" \
    --arg brief_v "$BRIEF_TEMPLATE_VERSION" \
    --arg arm "$arm" \
    --arg base "$base" \
    '{coder_model:$coder, reviewer_model:$reviewer, iter_cap:$iter_cap,
      brief_template_version:$brief_v, arm:$arm, base_sha:$base}'
}

# ===========================================================================
# brief construction  (the B-phase prompts)
# ===========================================================================
#
# coder_brief <plan> <problem-statement> — the strict-arm coder prompt.
#   = problem statement + the FROZEN objective gate (preregistration §3:
#     `zig build` clean AND `zig fmt` clean AND tests green).
# SCOPE: M1 builds only the strict arm. M2 adds eligibility/grouped arm shapes
# by varying which slice of the closure the brief dispatches; the seam is this
# function plus the arm parameter on run_cell.
coder_brief() {
  local plan="$1" problem="$2"
  cat <<EOF
You are an implementation agent in an isolated benchmark worktree.

PROBLEM STATEMENT (plan ${plan}):
${problem}

OBJECTIVE GATE (frozen, identical across all experimental arms — this is the
definition of "done"; reviewer approval is measured separately and does NOT
define done):
  1. \`zig build\` is clean (warnings are errors).
  2. \`zig fmt\` reports no changes.
  3. The test suite is green.

Implement the change, then run the gate. Report the gate outcome.
EOF
}

# reviewer_brief <plan> <problem-statement> — the reviewer prompt. The
# reviewer's verdict is MEASURED (M-ITER), not the definition of done.
reviewer_brief() {
  local plan="$1" problem="$2"
  cat <<EOF
You are a code reviewer in an isolated benchmark worktree.

The coder addressed this problem (plan ${plan}):
${problem}

Review the diff in the worktree. Respond with a verdict line of exactly
"VERDICT: approve" or "VERDICT: request-changes" followed by your reasoning.
Your verdict is recorded as a measurement; it does not gate the objective.
EOF
}

# reviewer_verdict <result-json> — extract approve|request-changes from the
# reviewer agent's result text (parses the "VERDICT:" line).
reviewer_verdict() {
  printf '%s' "$1" | jq -r '.result // ""' \
    | grep -oiE 'VERDICT:[[:space:]]*(approve|request-changes)' \
    | head -1 | awk '{print tolower($2)}'
}

# ===========================================================================
# 4. SLICE PLANNING  (arm-shape -> the set of slices the cell dispatches)
# ===========================================================================
#
# A "slice" is the unit a single coder works in a single worktree. The arm
# shape is entirely a function of how the plan's tasks are partitioned into
# slices and whether the slices run serially or concurrently:
#
#   strict      — N singleton slices (one task each), serial.
#   eligibility — the parallel-ELIGIBLE singletons run as one CONCURRENT
#                 cohort; the SERIALIZED remainder is N singleton slices, serial.
#   grouped     — the solver's co-located slices (>=1 task each), serial; one
#                 coder per slice over a shared worktree.
#
# Each planner prints, one slice per line, a compact JSON array of task ids
# (a "slice spec"). Concurrency is decided per-arm by the dispatcher, not here.

# plan_slices_strict <plan> <tasks_json> — one singleton slice per task.
plan_slices_strict() {
  printf '%s' "$2" | jq -c '.[] | [.]'
}

# plan_slices_eligibility <plan> — read `plan recommend-strategy`. Prints the
# eligible singletons first (these form the concurrent cohort), then a marker
# line "--", then the serialized singletons. The marker lets the dispatcher
# tell the cohort apart from the serial remainder. recommend-strategy is a
# cwd-derived READ (no --scope flag); it is run from the corpus repo.
plan_slices_eligibility() {
  local plan="$1" rec
  rec="$(pl_plan_json plan recommend-strategy "$plan" \
           --closure-source declared --json)"
  printf '%s' "$rec" | jq -c '.parallel_eligible[].id | [.]'
  printf -- '--\n'
  printf '%s' "$rec" | jq -c '.serialized[].id | [.]'
}

# plan_slices_grouped <plan> — read `groups recommend --solver <S>`. Each
# returned slice is a co-located task set worked by one coder in one worktree.
plan_slices_grouped() {
  local plan="$1"
  pl_plan_json groups recommend "$plan" --solver "$SOLVER" --json \
    | jq -c '.slices[].task_ids'
}

# ===========================================================================
# 5. THE SLICE WORKER + FAN-IN CONFLICT DETECTION
# ===========================================================================
#
# Cell-level state. run_cell sets these once; run_slice + the dispatchers read
# them. SEQ is the monotonic run_events sequence (UNIQUE(run_id,seq)); it is a
# script global so every slice in a multi-slice arm keeps appending without
# colliding. next_seq prints the next value and advances the counter.
CELL_RUN_UID=""
CELL_PLAN=""
CELL_ARM=""
CELL_BASE_SHA=""
CELL_PROBLEM=""
CELL_INTEG_BRANCH=""
INTEG_WORKTREE=""   # the checkout of CELL_INTEG_BRANCH; conflict-merge target
SEQ=0

# next_seq — advance the run_events sequence and publish the new value in the
# global S. It must NOT be used via command substitution (`$(next_seq)`): a
# subshell increment is lost in the parent, so every `--seq "$(next_seq)"` would
# re-emit seq 1 and trip UNIQUE(run_id,seq). The contract is: call `next_seq`,
# then read `$S`.
next_seq() { SEQ=$((SEQ + 1)); S="$SEQ"; }
S=0

# slice_worktree <slice_tag> — create a fresh detached worktree off the cell's
# base_sha and print its path. Each slice gets its own tree (strict: per task;
# eligibility: per cohort member; grouped: per co-located slice).
slice_worktree() {
  local tag="$1" wt="$WORKTREE_ROOT/${CELL_RUN_UID}/${tag}"
  rm -rf "$wt"; mkdir -p "$WORKTREE_ROOT/${CELL_RUN_UID}"
  git -C "$CORPUS_REPO_PATH" worktree add --detach "$wt" "$CELL_BASE_SHA" >&2
  printf '%s' "$wt"
}

# run_agents <worktree> <out_dir> — the B-phase for one slice: spawn the coder
# (capturing its session-cumulative token sample) then the reviewer (capturing
# its sample + parsed verdict). Writes three files into <out_dir>: coder.json,
# reviewer.json, verdict.txt. NO bench writes happen here — this is the part
# eligibility runs CONCURRENTLY as a background job, so it must touch only its
# own out_dir (the isolated SQLite is written serially by the parent at fan-in,
# never by a backgrounded cohort member).
run_agents() {
  local worktree="$1" out="$2"
  mkdir -p "$out"
  # A failed/empty agent result is a recorded MEASUREMENT, not a harness crash:
  # `|| true` on each step keeps `set -e` from aborting (and, when this runs as a
  # backgrounded cohort member, keeps `wait` from seeing a nonzero member exit).
  spawn_agent "$CODER_MODEL" "$worktree" \
    "$(coder_brief "$CELL_PLAN" "$CELL_PROBLEM")" >"$out/coder.json" 2>/dev/null \
    || printf '{"in":0,"out":0,"cache_in":0,"cache_out":0,"usd":0}' >"$out/coder.json"
  local reviewer_raw
  reviewer_raw="$(cd "$worktree" && reviewer_brief "$CELL_PLAN" "$CELL_PROBLEM" \
    | claude -p --output-format=json --model "$REVIEWER_MODEL" --add-dir "$worktree" 2>/dev/null || true)"
  printf '%s' "$reviewer_raw" | jq -c '{
    in:(.usage.input_tokens // 0), out:(.usage.output_tokens // 0),
    cache_in:(.usage.cache_read_input_tokens // 0),
    cache_out:(.usage.cache_creation_input_tokens // 0),
    usd:(.total_cost_usd // 0)}' >"$out/reviewer.json" 2>/dev/null \
    || printf '{"in":0,"out":0,"cache_in":0,"cache_out":0,"usd":0}' >"$out/reviewer.json"
  # reviewer_verdict's grep returns 1 when no VERDICT line is present; tolerate it.
  reviewer_verdict "$reviewer_raw" >"$out/verdict.txt" 2>/dev/null || true
  return 0
}

# fanin_conflict_check <worktree> <tasks_json> — the M-CONF instrument. At a
# slice's fan-in, attempt to integrate its committed work onto the per-arm
# integration branch (off the same base_sha). The harness does this with git
# directly because conflict detection is arm-shape-specific (it lives between
# the B-phase and the C-phase, decision D-HARNESS): a slice that committed
# nothing is a clean no-op; a slice that committed work is replayed onto the
# integration branch with `git merge --no-ff`. A merge failure (overlapping
# edits with an already-integrated slice) emits a `conflict` event carrying the
# slice's task ids — exactly what conflicts.sql counts. Returns 0 always (a
# conflict is a recorded measurement, not a harness error).
fanin_conflict_check() {
  local worktree="$1" tasks_json="$2"
  local slice_head
  # The slice's tip. The coder works detached; capture whatever it committed.
  slice_head="$(git -C "$worktree" rev-parse HEAD 2>/dev/null || true)"
  # A slice that committed nothing (HEAD still == base) integrates trivially.
  if [ -z "$slice_head" ] || [ "$slice_head" = "$CELL_BASE_SHA" ]; then
    git -C "$CORPUS_REPO_PATH" merge --no-edit --no-ff "$slice_head" \
      "$CELL_INTEG_BRANCH" >/dev/null 2>&1 || true  # no-op; nothing to merge
    return 0
  fi
  # Replay the slice's commits onto the integration branch. Operate in the
  # corpus repo's checkout of the integration branch (a dedicated worktree).
  if git -C "$INTEG_WORKTREE" merge --no-edit --no-ff "$slice_head" >/dev/null 2>&1; then
    log "fan-in: slice ${tasks_json} integrated cleanly onto ${CELL_INTEG_BRANCH}"
  else
    # Abort the half-applied merge to keep the integration branch usable for
    # the next slice, then RECORD the conflict (the measurement we are after).
    git -C "$INTEG_WORKTREE" merge --abort >/dev/null 2>&1 || true
    log "fan-in: slice ${tasks_json} CONFLICTS on ${CELL_INTEG_BRANCH} -> conflict event"
    next_seq
    pl bench event "$CELL_RUN_UID" --kind conflict --seq "$S" \
      --payload "$(jq -nc --arg arm "$CELL_ARM" --argjson tasks "$tasks_json" \
        --arg branch "$CELL_INTEG_BRANCH" \
        '{arm:$arm, task_ids:$tasks, integration_branch:$branch}')" >&2
  fi
}

# emit_slice_events <tasks_json> <out_dir> — serial bench writes for one slice
# at fan-in: slice_dispatch, the coder + reviewer token_samples, the measured
# reviewer_decision, then slice_fanin. Reads the agent artifacts run_agents
# left in <out_dir>. All writes go through the parent (never a bg job) so the
# isolated SQLite never sees concurrent writers.
emit_slice_events() {
  local tasks_json="$1" out="$2" coder_sample reviewer_sample verdict
  coder_sample="$(cat "$out/coder.json")"
  reviewer_sample="$(cat "$out/reviewer.json")"
  verdict="$(cat "$out/verdict.txt")"; [ -n "$verdict" ] || verdict="unknown"
  next_seq
  pl bench event "$CELL_RUN_UID" --kind slice_dispatch --seq "$S" \
    --payload "$(jq -nc --arg arm "$CELL_ARM" --argjson tasks "$tasks_json" \
      '{arm:$arm, tasks:$tasks}')" >&2
  next_seq; emit_token_sample "$CELL_RUN_UID" "$S" "coder" "$coder_sample" >&2
  next_seq; emit_token_sample "$CELL_RUN_UID" "$S" "reviewer" "$reviewer_sample" >&2
  next_seq
  pl bench event "$CELL_RUN_UID" --kind reviewer_decision --seq "$S" \
    --payload "$(jq -nc --arg v "$verdict" '{verdict:$v}')" >&2
  log "slice ${tasks_json}: reviewer verdict (measured) = ${verdict}"
}

# harvest_slice <worktree> <tasks_json> — per-task actual-touch harvest for one
# slice. `bench harvest` diffs the slice's worktree and writes kind=actual rows.
# Per-task even for a grouped (multi-task) slice, so per-task touch
# precision/recall stays computable (the co-located tasks share the slice diff,
# which is the documented grouped trade-off, decision D1).
harvest_slice() {
  local worktree="$1" tasks_json="$2" tid
  for tid in $(printf '%s' "$tasks_json" | jq -r '.[]'); do
    pl bench harvest "$CELL_RUN_UID" --task "$tid" --worktree "$worktree" >&2 || true
  done
}

# run_slice <slice_tag> <tasks_json> — drive ONE serial slice end-to-end:
# worktree -> B-phase agents -> fan-in (events + conflict check) -> harvest.
# strict, grouped, and the eligibility serial-remainder all call this.
run_slice() {
  local tag="$1" tasks_json="$2" worktree out
  title "slice ${tag} tasks=${tasks_json} (serial)"
  worktree="$(slice_worktree "$tag")"
  out="$TRANSCRIPT_ROOT/$CELL_RUN_UID/$tag"
  run_agents "$worktree" "$out"
  emit_slice_events "$tasks_json" "$out"
  fanin_conflict_check "$worktree" "$tasks_json"
  next_seq
  pl bench event "$CELL_RUN_UID" --kind slice_fanin --seq "$S" \
    --payload "$(jq -nc --arg arm "$CELL_ARM" --argjson tasks "$tasks_json" \
      '{arm:$arm, tasks:$tasks}')" >&2
  harvest_slice "$worktree" "$tasks_json"
}

# ===========================================================================
# 6. THE THREE ARM DISPATCHERS
# ===========================================================================
#
# Each dispatcher partitions the plan's tasks into slices and runs them in the
# arm's characteristic shape. run_cell brackets them with phase A (setup) and
# the final harvest/finish; the dispatchers only own the B-phase shape + fan-in.

# dispatch_strict <plan> <tasks_json> — N singleton slices, serial. With one
# task this is byte-for-byte M1's flow (one dispatch/agents/fanin/harvest).
dispatch_strict() {
  local plan="$1" tasks_json="$2" tslice i=0
  while IFS= read -r tslice; do
    [ -n "$tslice" ] || continue
    run_slice "t$(printf '%s' "$tslice" | jq -r '.[0]')" "$tslice"
    i=$((i + 1))
  done <<EOF
$(plan_slices_strict "$plan" "$tasks_json")
EOF
  log "strict: dispatched ${i} singleton slice(s) serially (M-BLAST=1)"
}

# dispatch_eligibility <plan> — the parallel-eligible singletons run as ONE
# CONCURRENT cohort (each its own worktree off base_sha, B-phase agents as
# background jobs, `wait` = the fan-in barrier); the serialized remainder runs
# strict-style afterward. Cohort bench writes are serialized at the barrier (the
# bg jobs only touch their own transcript dir), so the isolated SQLite never
# sees concurrent writers while still measuring true wall-clock parallelism.
dispatch_eligibility() {
  local plan="$1" line in_serial=0
  # Parallel indexed arrays track each cohort member: its task slice, worktree
  # tag, and background pid. (bash 3.2 indexed arrays are fine; the no-array
  # rule in this script's header is specifically about ASSOCIATIVE arrays.)
  local cohort_slices=() cohort_tags=() pids=()
  local n=0 tag wt out tslice

  while IFS= read -r line; do
    if [ "$line" = "--" ]; then in_serial=1; continue; fi
    [ -n "$line" ] || continue
    if [ "$in_serial" -eq 0 ]; then
      # eligible -> background B-phase; fan-in deferred to the wait barrier.
      tag="elig-$(printf '%s' "$line" | jq -r '.[0]')"
      wt="$(slice_worktree "$tag")"
      out="$TRANSCRIPT_ROOT/$CELL_RUN_UID/$tag"
      title "cohort member ${tag} tasks=${line} (concurrent B-phase)"
      ( run_agents "$wt" "$out" ) &
      pids[$n]="$!"
      cohort_slices[$n]="$line"
      cohort_tags[$n]="$tag"
      n=$((n + 1))
    else
      # serialized remainder -> ordinary serial slice (runs after the barrier
      # because the read loop only reaches these lines after the "--" marker,
      # which the recommend output always prints after the eligible subset).
      tslice="$line"
      run_slice "ser-$(printf '%s' "$tslice" | jq -r '.[0]')" "$tslice"
    fi
  done <<EOF
$(plan_slices_eligibility "$plan")
EOF

  # --- fan-in barrier for the concurrent cohort: WAIT, then emit serially ---
  if [ "$n" -gt 0 ]; then
    title "cohort fan-in barrier: waiting on ${n} concurrent member(s)"
    # Wait each pid individually so a single member's nonzero exit (a failed
    # agent is a recorded measurement) does not abort the cell under `set -e`.
    local _p
    for _p in "${pids[@]}"; do wait "$_p" || true; done
    local i ttasks
    for ((i = 0; i < n; i++)); do
      ttasks="${cohort_slices[$i]}"
      tag="${cohort_tags[$i]}"
      wt="$WORKTREE_ROOT/${CELL_RUN_UID}/${tag}"
      out="$TRANSCRIPT_ROOT/$CELL_RUN_UID/$tag"
      emit_slice_events "$ttasks" "$out"
      fanin_conflict_check "$wt" "$ttasks"
      next_seq
      pl bench event "$CELL_RUN_UID" --kind slice_fanin --seq "$S" \
        --payload "$(jq -nc --arg arm "$CELL_ARM" --argjson tasks "$ttasks" \
          '{arm:$arm, tasks:$tasks, cohort:true}')" >&2
      harvest_slice "$wt" "$ttasks"
    done
    log "eligibility: ${n} eligible task(s) ran CONCURRENTLY then fanned in"
  else
    log "eligibility: no parallel-eligible tasks; everything ran serially"
  fi
}

# dispatch_grouped <plan> — the solver's co-located slices, serial. One coder
# works each whole slice in ONE shared worktree; one slice_dispatch/slice_fanin
# per SLICE; harvest is per-task-in-slice (M-BLAST = slice size).
dispatch_grouped() {
  local plan="$1" tslice i=0 n
  while IFS= read -r tslice; do
    [ -n "$tslice" ] || continue
    n="$(printf '%s' "$tslice" | jq 'length')"
    run_slice "g${i}-n${n}" "$tslice"
    i=$((i + 1))
  done <<EOF
$(plan_slices_grouped "$plan")
EOF
  log "grouped: dispatched ${i} co-located slice(s) serially (solver=${SOLVER}, M-BLAST=slice size)"
}

# ===========================================================================
# 7. THE CELL FLOW
# ===========================================================================
#
# run_cell <plan> <arm> <rep> — drive ONE cell end-to-end: resolve metadata +
# base_sha, open the run (ritual phase A), dispatch the arm shape, finish. In
# --dry-run it prints each arm's DISTINCT slice/worktree/dispatch shape and
# mutates nothing.
run_cell() {
  local plan="$1" arm="$2" rep="$3"
  local scope problem paths_list base_sha run_uid chash cjson tasks_json setup_args

  title "cell (plan=${plan}, arm=${arm}, rep=${rep})"

  # --- resolve plan metadata + declared closure ---
  scope="$(plan_scope "$plan")"
  problem="$(pl_plan_json plan show "$plan" --json | jq -r '.summary // .title')"
  paths_list="$(declared_paths "$plan")"
  log "scope: ${scope}"
  log "declared paths (${arm} closure):"
  if [ -n "$paths_list" ]; then
    printf '%s\n' "$paths_list" | while IFS= read -r p; do log "    $p"; done
  else
    log "    (none declared)"
  fi

  # --- base-SHA selection + base-fidelity verification gate (M1, reused) ---
  if [ -n "$paths_list" ]; then
    # shellcheck disable=SC2046  # intentional word-split of the path list into argv
    base_sha="$(pick_base_sha "$CORPUS_REPO_PATH" $paths_list)"
    log "base_sha (parent of first touching commit): ${base_sha}"
    # shellcheck disable=SC2046
    verify_feature_absent "$CORPUS_REPO_PATH" "$base_sha" $paths_list
  else
    base_sha="$(git -C "$CORPUS_REPO_PATH" rev-parse HEAD)"
    log "base_sha (no declared paths; using corpus HEAD): ${base_sha}"
  fi

  # --- cell identity ---
  run_uid="$(mint_run_uid "$plan" "$arm" "$rep")"
  chash="$(config_hash "$arm")"
  cjson="$(config_json "$arm" "$base_sha")"
  log "run_uid:     ${run_uid}"
  log "config_hash: ${chash}"

  # --- the cell's full task set (for the ritual setup + base for slicing) ---
  if [ -n "$ARG_TASKS" ]; then
    tasks_json="$(printf '%s' "$ARG_TASKS" | jq -Rc 'split(",") | map(tonumber)')"
  else
    tasks_json="$(pl_plan_json task list --plan "$plan" --scope "$scope" --json \
                    | jq -c '[.[].id]')"
  fi
  log "tasks:       ${tasks_json}"

  # --- ritual phase-A --args contract (bench_run_ritual.lua ctx.args) ---
  setup_args="$(jq -nc \
    --arg run_uid "$run_uid" --argjson plan_id "$plan" \
    --arg base_sha "$base_sha" --arg config_hash "$chash" \
    --arg arm "$arm" --arg corpus_repo "$CORPUS_REPO_NAME" \
    --arg config_json "$cjson" --argjson tasks "$tasks_json" \
    '{run_uid:$run_uid, plan_id:$plan_id, base_sha:$base_sha,
      config_hash:$config_hash, arm:$arm, corpus_repo:$corpus_repo,
      config_json:$config_json, tasks:$tasks}')"

  # =======================================================================
  # --dry-run: print each arm's DISTINCT shape, mutate NOTHING, spawn NOTHING.
  # =======================================================================
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_run_arm "$plan" "$arm" "$run_uid" "$base_sha" "$tasks_json" "$setup_args"
    return 0
  fi

  # =======================================================================
  # LIVE cell flow.
  # =======================================================================

  # --- export cell-level state the dispatchers + run_slice read ---
  CELL_RUN_UID="$run_uid"; CELL_PLAN="$plan"; CELL_ARM="$arm"
  CELL_BASE_SHA="$base_sha"; CELL_PROBLEM="$problem"
  CELL_INTEG_BRANCH="bench-integ/${run_uid}"
  SEQ=0; S=0
  mkdir -p "$WORKTREE_ROOT/$run_uid" "$TRANSCRIPT_ROOT/$run_uid"

  # --- self-heal stale worktree registrations from a crashed prior run. The
  #     dispatchers rm -rf each slice dir before `git worktree add`; without a
  #     prune, git still has the deleted path REGISTERED and refuses to re-add.
  #     (Full resume/cleanup is M3; this keeps a re-run from wedging.) ---
  git -C "$CORPUS_REPO_PATH" worktree prune >/dev/null 2>&1 || true

  # --- per-arm integration branch + its worktree (the conflict-merge target) ---
  INTEG_WORKTREE="$WORKTREE_ROOT/$run_uid/_integration"
  rm -rf "$INTEG_WORKTREE"
  git -C "$CORPUS_REPO_PATH" branch -f "$CELL_INTEG_BRANCH" "$base_sha" >&2
  git -C "$CORPUS_REPO_PATH" worktree add "$INTEG_WORKTREE" "$CELL_INTEG_BRANCH" >&2

  # --- phase A: setup (reset + bench start + declared snapshot). Run against a
  #     throwaway scratch worktree off base_sha (the ritual resets it; the real
  #     per-slice work happens in the slice worktrees the dispatchers cut). ---
  title "phase A: setup"
  local scratch_wt="$WORKTREE_ROOT/$run_uid/_setup"
  rm -rf "$scratch_wt"
  git -C "$CORPUS_REPO_PATH" worktree add --detach "$scratch_wt" "$base_sha" >&2
  execute_phase setup "$scratch_wt" "$setup_args" >&2

  # --- phase B + fan-in: dispatch the arm's characteristic slice shape ---
  title "phase B: arm dispatch (${arm})"
  case "$arm" in
    strict)      dispatch_strict "$plan" "$tasks_json" ;;
    eligibility) dispatch_eligibility "$plan" ;;
    grouped)     dispatch_grouped "$plan" ;;
    *) err "run_cell: unknown arm '${arm}'" ;;
  esac

  # --- phase C: finish the run (harvest already happened per-slice). The run is
  #     joinable: declared touches (snapshotted at start) + per-task actual
  #     touches (harvested per slice) + the full event journal. ---
  title "phase C: finish"
  pl bench finish "$run_uid" --status completed >&2

  # --- clean joinable run: print the final record on stdout ---
  title "cell complete: ${run_uid}"
  pl bench show "$run_uid" --json
}

# dry_run_arm <plan> <arm> <run_uid> <base_sha> <tasks_json> <setup_args> —
# print the arm's DISTINCT worktree/dispatch shape + the slices/cohorts it
# would form. Spawns nothing, mutates nothing.
dry_run_arm() {
  local plan="$1" arm="$2" run_uid="$3" base_sha="$4" tasks_json="$5" setup_args="$6"
  title "DRY-RUN plan for cell ${run_uid} (arm=${arm})"
  cat >&2 <<EOF
  WOULD open run (ritual phase A, setup) on a scratch worktree off ${base_sha}:
    ${PLANAR_EXECUTE_BIN} run ${RITUAL_LUA} --phase setup --worktree <scratch> \\
        --args '${setup_args}'
  WOULD create integration branch bench-integ/${run_uid} off ${base_sha}
        (the per-arm conflict-merge target; M-CONF).
EOF
  case "$arm" in
    strict)
      cat >&2 <<EOF

  ARM=strict: N SINGLETON slices, SERIAL (M-BLAST=1).
  Slices (one isolated worktree each off ${base_sha}):
EOF
      plan_slices_strict "$plan" "$tasks_json" | while IFS= read -r s; do
        printf '    slice %s  -> worktree .../%s/t%s  -> coder+reviewer -> merge onto integ (conflict?) -> slice_fanin -> harvest per-task\n' \
          "$s" "$run_uid" "$(printf '%s' "$s" | jq -r '.[0]')" >&2
      done
      ;;
    eligibility)
      cat >&2 <<EOF

  ARM=eligibility: parallel-ELIGIBLE subset CONCURRENT (one worktree each,
  agents as bg jobs, wait = fan-in barrier), then SERIALIZED remainder serial.
  From: plan recommend-strategy ${plan} --closure-source declared
EOF
      local in_serial=0 s
      while IFS= read -r s; do
        if [ "$s" = "--" ]; then printf '    --- barrier (wait the cohort) ---\n' >&2; in_serial=1; continue; fi
        [ -n "$s" ] || continue
        if [ "$in_serial" -eq 0 ]; then
          printf '    [concurrent] slice %s -> worktree .../%s/elig-%s -> bg coder+reviewer\n' \
            "$s" "$run_uid" "$(printf '%s' "$s" | jq -r '.[0]')" >&2
        else
          printf '    [serial]     slice %s -> worktree .../%s/ser-%s -> coder+reviewer\n' \
            "$s" "$run_uid" "$(printf '%s' "$s" | jq -r '.[0]')" >&2
        fi
      done <<EOF2
$(plan_slices_eligibility "$plan")
EOF2
      ;;
    grouped)
      cat >&2 <<EOF

  ARM=grouped: solver co-located slices, SERIAL; one coder per SHARED-worktree
  slice; harvest per-task-in-slice (M-BLAST=slice size). solver=${SOLVER}.
  From: groups recommend ${plan} --solver ${SOLVER}
EOF
      local i=0 s
      while IFS= read -r s; do
        [ -n "$s" ] || continue
        printf '    slice #%s %s (size %s) -> ONE shared worktree .../%s/g%s -> one coder -> merge onto integ (conflict?) -> slice_fanin -> harvest EACH task\n' \
          "$i" "$s" "$(printf '%s' "$s" | jq 'length')" "$run_uid" "$i" >&2
        i=$((i + 1))
      done <<EOF3
$(plan_slices_grouped "$plan")
EOF3
      ;;
  esac
  cat >&2 <<EOF

  WOULD finish: ${PLANAR_BIN} bench finish ${run_uid} --status completed
  WOULD then:   ${PLANAR_BIN} bench show ${run_uid} --json   (clean joinable run)
EOF
  title "DRY-RUN complete (no spawns, no mutations)"
}

# ===========================================================================
# argv + main
# ===========================================================================

usage() {
  sed -n '2,59p' "$0" | sed 's/^# \?//'
}

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --plan)     ARG_PLAN="$2"; shift 2 ;;
      --rep)      ARG_REP="$2"; shift 2 ;;
      --tasks)    ARG_TASKS="$2"; shift 2 ;;
      --arm)      ARG_ARM="$2"; shift 2 ;;
      --solver)   SOLVER="$2"; shift 2 ;;
      --dry-run)  DRY_RUN=1; shift ;;
      -h|--help)  usage; exit 0 ;;
      *) err "unknown flag: $1 (try --help)" ;;
    esac
  done
  [ -n "$ARG_PLAN" ] || err "--plan <id> is required (try --help)"
  case "$ARG_ARM" in
    strict|eligibility|grouped) : ;;
    *) err "--arm must be one of: strict | eligibility | grouped (got '$ARG_ARM')" ;;
  esac
}

main() {
  require_tool jq
  require_tool git
  require_tool shasum
  parse_args "$@"

  title "bench-matrix: M2 arm-shape driver"
  log "PLANAR_DB:          $PLANAR_DB"
  log "PLANAR_CONFIG_PATH: $PLANAR_CONFIG_PATH"
  log "corpus repo:        $CORPUS_REPO_PATH ($CORPUS_REPO_NAME)"
  log "coder/reviewer:     $CODER_MODEL / $REVIEWER_MODEL"
  log "iteration cap:      $ITER_CAP"
  log "arm:                $ARG_ARM"
  [ "$ARG_ARM" = "grouped" ] && log "solver:             $SOLVER"
  [ "$DRY_RUN" -eq 1 ] && log "MODE:               --dry-run (no spawns, no mutations)"

  # SCOPE: M2 drives exactly ONE cell of the requested arm, rep as given. The
  # matrix loop (arms x plans x reps with interleaving + spend ceiling +
  # resume) is M3 and wraps run_cell in an outer loop here.
  run_cell "$ARG_PLAN" "$ARG_ARM" "$ARG_REP"
}

main "$@"
