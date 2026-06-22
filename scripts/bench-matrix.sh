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
# SCOPE (M1, foundation): single-cell strict-arm driver + telemetry capture
# + base-SHA selection/verification gate + --dry-run. The code is structured
# as functions with a clear `main` so later milestones slot in:
#   * M2 — the eligibility / grouped arm shapes (build_arm_brief / arm closure).
#   * M3 — the matrix loop, arm interleaving, resume-via-run_uid, spend ceiling.
# Those are deliberately NOT implemented here; see the SCOPE markers below.
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
#   scripts/bench-matrix.sh --plan <id> [--rep 1] [--dry-run]
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

# --- the corpus repo under measurement. Defaults to THIS planar checkout
#     (the experiment measures planar's own decomposition). Override for a
#     different corpus member. The cycle worktree is cut from here. ---
CORPUS_REPO_PATH="${BENCH_CORPUS_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CORPUS_REPO_NAME="${BENCH_CORPUS_REPO_NAME:-planar}"

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
  task_ids="$(pl_json task list --plan "$plan" --scope "$scope" --json \
                | jq -r '.[].id')"
  : "$scope"  # scope is used for the task-list read; touches list needs none
  for tid in $task_ids; do
    # `task touches list` is a read (no --scope flag); its JSON shape is
    # {task_id, repos:[...], paths:[{repo, path}]}. Pull .paths[].path.
    pl_json task touches list "$tid" --json 2>/dev/null \
      | jq -r '.paths[]?.path // empty' 2>/dev/null || true
  done | sort -u
}

# plan_scope <plan> — resolve the --scope slug a plan's writes need (the
# cwd-derive guard refuses bare reads from outside a registered project).
# Derived from scope_kind/scope_id on `plan show`.
plan_scope() {
  local plan="$1" kind id slug
  local pj
  pj="$(pl_json plan show "$plan" --json)"
  kind="$(printf '%s' "$pj" | jq -r '.scope_kind')"
  id="$(printf '%s' "$pj" | jq -r '.scope_id')"
  if [ "$kind" = "global" ]; then
    printf 'global'
    return 0
  fi
  # association scope: map scope_id -> slug as "project:<slug>" / "<slug>".
  slug="$(pl_json assoc list --json | jq -r --argjson id "$id" \
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
# 4. THE CELL FLOW
# ===========================================================================
#
# run_cell <plan> <arm> <rep> — drive ONE cell end-to-end. In --dry-run it
# prints the full plan (base_sha, worktrees, the agent invocations + bench
# calls it WOULD make) and mutates nothing.
run_cell() {
  local plan="$1" arm="$2" rep="$3"
  local scope problem paths_list base_sha worktree run_uid chash cjson
  local seq=0

  title "cell (plan=${plan}, arm=${arm}, rep=${rep})"

  # --- resolve plan metadata + declared closure ---
  scope="$(plan_scope "$plan")"
  problem="$(pl_json plan show "$plan" --json | jq -r '.summary // .title')"
  paths_list="$(declared_paths "$plan")"
  log "scope: ${scope}"
  log "declared paths (${arm} closure):"
  if [ -n "$paths_list" ]; then
    printf '%s\n' "$paths_list" | while IFS= read -r p; do log "    $p"; done
  else
    log "    (none declared)"
  fi

  # --- 3. base-SHA selection + base-fidelity verification gate ---
  if [ -n "$paths_list" ]; then
    # shellcheck disable=SC2046  # intentional word-split of the path list into argv
    base_sha="$(pick_base_sha "$CORPUS_REPO_PATH" $paths_list)"
    log "base_sha (parent of first touching commit): ${base_sha}"
    # shellcheck disable=SC2046
    verify_feature_absent "$CORPUS_REPO_PATH" "$base_sha" $paths_list
  else
    # No declared paths: fall back to the corpus HEAD (still a clean base for a
    # fixture cell). A real corpus plan always declares paths; this branch keeps
    # a minimal-fixture --dry-run / smoke runnable.
    base_sha="$(git -C "$CORPUS_REPO_PATH" rev-parse HEAD)"
    log "base_sha (no declared paths; using corpus HEAD): ${base_sha}"
  fi

  # --- cell identity ---
  run_uid="$(mint_run_uid "$plan" "$arm" "$rep")"
  chash="$(config_hash "$arm")"
  cjson="$(config_json "$arm" "$base_sha")"
  worktree="$WORKTREE_ROOT/$run_uid"
  log "run_uid:     ${run_uid}"
  log "config_hash: ${chash}"
  log "worktree:    ${worktree}"

  # --- task ids the slice covers (for the ritual + harvest) ---
  local tasks_json
  if [ -n "$ARG_TASKS" ]; then
    tasks_json="$(printf '%s' "$ARG_TASKS" | jq -Rc 'split(",") | map(tonumber)')"
  else
    tasks_json="$(pl_json task list --plan "$plan" --scope "$scope" --json \
                    | jq -c '[.[].id]')"
  fi
  log "tasks:       ${tasks_json}"

  # --- ritual --args contract (bench_run_ritual.lua ctx.args) ---
  local setup_args measure_args
  setup_args="$(jq -nc \
    --arg run_uid "$run_uid" --argjson plan_id "$plan" \
    --arg base_sha "$base_sha" --arg config_hash "$chash" \
    --arg arm "$arm" --arg corpus_repo "$CORPUS_REPO_NAME" \
    --arg config_json "$cjson" --argjson tasks "$tasks_json" \
    '{run_uid:$run_uid, plan_id:$plan_id, base_sha:$base_sha,
      config_hash:$config_hash, arm:$arm, corpus_repo:$corpus_repo,
      config_json:$config_json, tasks:$tasks}')"
  measure_args="$(jq -nc \
    --arg run_uid "$run_uid" --arg worktree "$worktree" \
    --argjson tasks "$tasks_json" --arg status "completed" \
    '{run_uid:$run_uid, worktree:$worktree, tasks:$tasks, status:$status}')"

  # =======================================================================
  # 5. --dry-run: print the full plan, mutate NOTHING, spawn NOTHING.
  # =======================================================================
  if [ "$DRY_RUN" -eq 1 ]; then
    title "DRY-RUN plan for cell ${run_uid}"
    cat >&2 <<EOF
  WOULD create cycle worktree:
    git -C ${CORPUS_REPO_PATH} worktree add --detach ${worktree} ${base_sha}

  WOULD run ritual phase A (setup):
    ${PLANAR_EXECUTE_BIN} run ${RITUAL_LUA} --phase setup --worktree ${worktree} \\
        --args '${setup_args}'

  WOULD emit slice_dispatch event:
    ${PLANAR_BIN} bench event ${run_uid} --kind slice_dispatch --seq 1 --payload '{"arm":"${arm}","tasks":${tasks_json}}'

  WOULD spawn CODER agent (model=${CODER_MODEL}) in ${worktree}:
    coder_brief | claude -p --output-format=json --model ${CODER_MODEL} --permission-mode acceptEdits --add-dir ${worktree}
      brief: coder_brief(plan=${plan})  [problem + frozen objective gate; fed on stdin]
  WOULD emit token_sample (seq 2, role=coder) from the coder's session-cumulative usage.

  WOULD spawn REVIEWER agent (model=${REVIEWER_MODEL}, iteration cap ${ITER_CAP}):
    reviewer_brief | claude -p --output-format=json --model ${REVIEWER_MODEL} --add-dir ${worktree}
      brief: reviewer_brief(plan=${plan})  [fed on stdin]
  WOULD emit token_sample (seq 3, role=reviewer) + record VERDICT (measured, not gating).

  WOULD emit slice_fanin event:
    ${PLANAR_BIN} bench event ${run_uid} --kind slice_fanin --seq 4 --payload '{...}'

  WOULD run ritual phase C (measure):
    ${PLANAR_EXECUTE_BIN} run ${RITUAL_LUA} --phase measure --worktree ${worktree} \\
        --args '${measure_args}'

  WOULD then: ${PLANAR_BIN} bench show ${run_uid} --json   (clean joinable run)
EOF
    title "DRY-RUN complete (no spawns, no mutations)"
    return 0
  fi

  # =======================================================================
  # LIVE cell flow.
  # =======================================================================

  # --- create the cycle worktree off base_sha ---
  rm -rf "$worktree"; mkdir -p "$WORKTREE_ROOT" "$TRANSCRIPT_ROOT/$run_uid"
  log "creating cycle worktree off ${base_sha}"
  git -C "$CORPUS_REPO_PATH" worktree add --detach "$worktree" "$base_sha" >&2

  # --- phase A: setup (reset + bench start + declared snapshot) ---
  title "phase A: setup"
  execute_phase setup "$worktree" "$setup_args" >&2

  # --- slice_dispatch event ---
  seq=$((seq + 1))
  pl bench event "$run_uid" --kind slice_dispatch --seq "$seq" \
    --payload "$(jq -nc --arg arm "$arm" --argjson tasks "$tasks_json" '{arm:$arm,tasks:$tasks}')" >&2

  # --- B-phase: coder ---
  title "phase B: coder agent (${CODER_MODEL})"
  local coder_sample
  coder_sample="$(spawn_agent "$CODER_MODEL" "$worktree" "$(coder_brief "$plan" "$problem")")"
  log "coder token_sample: ${coder_sample}"
  seq=$((seq + 1))
  emit_token_sample "$run_uid" "$seq" "coder" "$coder_sample" >&2

  # --- B-phase: reviewer (verdict measured, iteration cap is the spend bound) ---
  title "phase B: reviewer agent (${REVIEWER_MODEL})"
  local reviewer_raw reviewer_sample verdict
  reviewer_raw="$(cd "$worktree" && reviewer_brief "$plan" "$problem" | claude -p \
    --output-format=json --model "$REVIEWER_MODEL" --add-dir "$worktree")"
  reviewer_sample="$(printf '%s' "$reviewer_raw" | jq -c '{
    in:(.usage.input_tokens // 0), out:(.usage.output_tokens // 0),
    cache_in:(.usage.cache_read_input_tokens // 0),
    cache_out:(.usage.cache_creation_input_tokens // 0),
    usd:(.total_cost_usd // 0)}')"
  verdict="$(reviewer_verdict "$reviewer_raw")"
  [ -n "$verdict" ] || verdict="unknown"
  log "reviewer verdict (measured): ${verdict}"
  log "reviewer token_sample: ${reviewer_sample}"
  seq=$((seq + 1))
  emit_token_sample "$run_uid" "$seq" "reviewer" "$reviewer_sample" >&2
  seq=$((seq + 1))
  pl bench event "$run_uid" --kind reviewer_decision --seq "$seq" \
    --payload "$(jq -nc --arg v "$verdict" '{verdict:$v}')" >&2

  # --- slice_fanin event ---
  seq=$((seq + 1))
  pl bench event "$run_uid" --kind slice_fanin --seq "$seq" \
    --payload "$(jq -nc --arg arm "$arm" '{arm:$arm}')" >&2

  # --- phase C: measure (harvest + finish) ---
  title "phase C: measure"
  execute_phase measure "$worktree" "$measure_args" >&2

  # --- clean joinable run: print the final record on stdout ---
  title "cell complete: ${run_uid}"
  pl bench show "$run_uid" --json
}

# ===========================================================================
# argv + main
# ===========================================================================

usage() {
  sed -n '2,40p' "$0" | sed 's/^# \?//'
}

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --plan)     ARG_PLAN="$2"; shift 2 ;;
      --rep)      ARG_REP="$2"; shift 2 ;;
      --tasks)    ARG_TASKS="$2"; shift 2 ;;
      --dry-run)  DRY_RUN=1; shift ;;
      -h|--help)  usage; exit 0 ;;
      *) err "unknown flag: $1 (try --help)" ;;
    esac
  done
  [ -n "$ARG_PLAN" ] || err "--plan <id> is required (try --help)"
}

main() {
  require_tool jq
  require_tool git
  require_tool shasum
  parse_args "$@"

  title "bench-matrix: M1 single-cell driver"
  log "PLANAR_DB:          $PLANAR_DB"
  log "PLANAR_CONFIG_PATH: $PLANAR_CONFIG_PATH"
  log "corpus repo:        $CORPUS_REPO_PATH ($CORPUS_REPO_NAME)"
  log "coder/reviewer:     $CODER_MODEL / $REVIEWER_MODEL"
  log "iteration cap:      $ITER_CAP"
  [ "$DRY_RUN" -eq 1 ] && log "MODE:               --dry-run (no spawns, no mutations)"

  # SCOPE: M1 drives exactly ONE cell, the strict arm, rep as given. The
  # matrix loop (arms x plans x reps with interleaving + spend ceiling +
  # resume) is M3 and wraps run_cell in an outer loop here.
  run_cell "$ARG_PLAN" "strict" "$ARG_REP"
}

main "$@"
