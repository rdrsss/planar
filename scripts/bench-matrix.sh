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
# SCOPE (M3): the matrix loop wrapping run_cell — the (rep,plan,arm) schedule
# with per-(rep,plan) RANDOMIZED arm interleaving (§4), N reps, paired
# config_hash per (plan,rep) so the 3 arms join as paired comparisons (§4
# blocking), DETERMINISTIC run_uids for additive resume (skip completed, abort +
# retry crashed; §4 idempotency), and a spend-ceiling stop (§9). --dry-run over
# the matrix prints the full interleaved schedule + per-plan base_sha + run_uids
# + estimated total spend — the operator pre-approval surface.
#
# M4 IMPLEMENTED: trap-based worktree cleanup, per-cell timeout/kill, structured
# per-cell JSONL logging, and a per-cell retry cap. dispatch_cell is the decorated
# entry point; spawn_agent wraps every claude invocation with a watchdog.
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
#   Single cell (M1/M2):
#     scripts/bench-matrix.sh --plan <id> [--arm strict|eligibility|grouped] [--rep 1] [--dry-run]
#     scripts/bench-matrix.sh --plan <id> --arm grouped --solver mtkahypar --dry-run
#     scripts/bench-matrix.sh --plan <id> --tasks 101,102 --dry-run
#     scripts/bench-matrix.sh --plan <id> --base <sha> --dry-run   # override base for modify-features
#   Matrix (M3): sweep plans x arms x reps, interleaved, paired, resumable.
#     scripts/bench-matrix.sh --matrix --plans 635,699 --reps 3 --dry-run
#     scripts/bench-matrix.sh --matrix --plans 635,699 --reps 3 --ceiling 50.00
#     scripts/bench-matrix.sh --matrix --plans 659,668,678 --reps 5 \
#       --bases 659:fcb167a,668:274b6f6,678:32061f7   # per-plan base overrides
#   --matrix flags: --plans <ids>  --reps <N>  --ceiling <USD>  (also honors
#     BENCH_CORPUS_PLANS / BENCH_N_REPS / BENCH_CEILING). Re-running --matrix
#     resumes additively: completed cells SKIP, crashed cells abort + retry.
#   Base override flags (for modify-feature corpora where paths pre-exist):
#     --base <sha>             single-cell: use <sha> as base, skip pick_base_sha.
#     --bases <plan:sha,...>   matrix: per-plan base map; mirrors --plans convention.
#     BENCH_BASES env var      same as --bases; honoured before flag parsing.
#   When an override base is in effect the file-existence base-fidelity gate is
#   softened: pre-existing declared paths are LOGGED (not aborted) because the
#   operator asserts base fidelity by construction (base = parent of earliest
#   task commit, not a pick_base_sha walk).
#
# M4 env vars (all optional; follow BENCH_* convention):
#   BENCH_AGENT_TIMEOUT   — per-agent wall-clock deadline in seconds; the watchdog
#                           kills the entire agent process group on breach.
#                           Default: 600 (10 min). Set to 0 to disable.
#   BENCH_CELL_RETRY_CAP  — max crash-retries per cell before it is marked
#                           permanently failed and skipped. Default: 2.
#   BENCH_LOG_ROOT        — directory for per-cell JSONL structured logs.
#                           Default: $BENCH_HOME/cell-logs
#
#     scripts/bench-matrix.sh --help

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

# ---------------------------------------------------------------------------
# M3 matrix config (operator-editable; frozen into config_hash where noted).
# ---------------------------------------------------------------------------
# --- the corpus plan-list the matrix sweeps. Space- or comma-separated plan
#     ids. Empty in single-cell mode (--plan drives one cell instead). ---
CORPUS_PLANS="${BENCH_CORPUS_PLANS:-}"

# --- repetitions per cell (preregistration §4: N = 3–5; default 3). Every
#     (plan,arm) pair runs N times; medians + spread are reported, never a
#     single run. ---
N_REPS="${BENCH_N_REPS:-3}"

# --- the three experimental arms, in canonical order. The schedule RANDOMIZES
#     the per-(rep,plan) order to randomize model drift over calendar time
#     (§4); this is just the membership list. ---
ARMS="strict eligibility grouped"

# --- spend ceiling (USD, preregistration §9). Cumulative spend is the SUM of
#     the `usd` field over every token_sample payload in the experiment DB
#     (excluding aborted runs). Before each cell the driver checks
#     cumulative + estimated_cell_cost <= CEILING; on breach it STOPS cleanly,
#     dispatching nothing new and leaving completed runs intact. Frozen into
#     config_hash (a different budget is a different experiment). ---
CEILING="${BENCH_CEILING:-50.00}"

# --- first-cell cost estimate (USD). Once cells complete, the driver estimates
#     the next cell's cost from the observed per-cell average; this default
#     seeds the estimate before any cell has run. ---
CELL_COST_DEFAULT="${BENCH_CELL_COST_DEFAULT:-2.00}"

# ---------------------------------------------------------------------------
# M4 config (operator-editable via env).
# ---------------------------------------------------------------------------
# --- per-agent timeout in seconds. The watchdog kills the entire claude process
#     group after this deadline. 0 = disabled. ---
AGENT_TIMEOUT="${BENCH_AGENT_TIMEOUT:-600}"

# --- per-cell crash retry cap. After this many abort+retry cycles the cell is
#     marked permanently failed and skipped, not retried again. ---
CELL_RETRY_CAP="${BENCH_CELL_RETRY_CAP:-2}"

# --- per-cell JSONL structured log directory (created on first write). ---
LOG_ROOT="${BENCH_LOG_ROOT:-}"   # resolved lazily below after BENCH_HOME is stable

# ---------------------------------------------------------------------------
# Base-override config (operator-supplied per-plan bases for modify-features).
# ---------------------------------------------------------------------------
# --- per-plan base SHA overrides. Format: "plan_id:sha,plan_id:sha,...". The
#     matrix driver reads this to skip pick_base_sha for plans whose real base
#     is known (modify-feature corpus where path-existence walk would go back to
#     genesis). The override base is validated as a real commit; the file-
#     existence base-fidelity gate is SOFTENED (logged, not hard-fail) when an
#     override is in effect. Mirrors the --plans/BENCH_CORPUS_PLANS convention. ---
BENCH_BASES="${BENCH_BASES:-}"   # env var; also settable via --bases flag

# --- runtime state (parsed from argv) ---
ARG_PLAN=""
ARG_REP=1
ARG_TASKS=""        # optional comma-list; empty = all plan tasks
ARG_ARM="strict"    # strict | eligibility | grouped (M2: the three arm shapes)
ARG_MATRIX=0        # 1 = M3 matrix mode (sweep CORPUS_PLANS x ARMS x N_REPS)
ARG_BASE=""         # single-cell: explicit base SHA (skips pick_base_sha)
DRY_RUN=0

# --- M3 cell-identity injection (set per-cell by the matrix driver; empty in
#     single-cell M1/M2 mode so run_cell falls back to its own minting). ---
CELL_RUN_UID_OVERRIDE=""
CELL_CHASH_OVERRIDE=""

# --- M4 runtime: the in-flight agent process group (tracked for trap + watchdog).
#     Set by spawn_agent_watchdog; cleared on return. 0 = none in flight. ---
_INFLIGHT_AGENT_PGID=0

# --- M4 runtime: the in-flight cell's slice/integration/scratch dir list for the
#     trap cleanup. Populated by run_cell; cleared on clean completion. ---
_INFLIGHT_CELL_UID=""
_INFLIGHT_CELL_DIRS=()   # bash indexed array (bash 3.2 ok; NOT associative)

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

# ===========================================================================
# M4 helpers: watchdog, trap cleanup, structured cell logging
# ===========================================================================

# ---------------------------------------------------------------------------
# cell_log <event_type> <json_fields_fragment>
#   Appends one JSONL record to $LOG_ROOT/<cell_uid>.jsonl (creates the file and
#   directory on first write). Each record carries a timestamp, the cell identity
#   globals, the event_type, and any caller-supplied fields merged in.
#   No-ops when DRY_RUN=1 or when LOG_ROOT is empty (should never be, but guard).
#   Safe to call under set -e: errors are swallowed (logging must never abort a
#   cell; it is additive / best-effort).
# ---------------------------------------------------------------------------
cell_log() {
  local event_type="$1" extra_fields="${2:-}"
  [ "$DRY_RUN" -eq 1 ] && return 0
  # Resolve LOG_ROOT lazily (needs BENCH_HOME to be stable, which it is by now).
  [ -n "$LOG_ROOT" ] || LOG_ROOT="$BENCH_HOME/cell-logs"
  local uid="${_INFLIGHT_CELL_UID:-${CELL_RUN_UID:-unknown}}"
  local ts; ts="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || printf 'unknown')"
  local logfile="$LOG_ROOT/${uid}.jsonl"
  mkdir -p "$LOG_ROOT" 2>/dev/null || true
  # Build record: merge base fields + extra_fields (if valid JSON object fragment).
  local base; base="$(jq -nc \
    --arg ts "$ts" \
    --arg ev "$event_type" \
    --arg uid "$uid" \
    --arg plan "${CELL_PLAN:-}" \
    --arg arm "${CELL_ARM:-}" \
    '{ts:$ts, event:$ev, run_uid:$uid, plan:$plan, arm:$arm}')" 2>/dev/null || return 0
  if [ -n "$extra_fields" ]; then
    printf '%s\n' "$base" | jq -c --argjson ex "$extra_fields" '. + $ex' \
      >>"$logfile" 2>/dev/null || true
  else
    printf '%s\n' "$base" >>"$logfile" 2>/dev/null || true
  fi
}

# ---------------------------------------------------------------------------
# _cleanup_inflight — kill the in-flight agent process group (if any) and
# remove the current cell's worktree directories. Called by the EXIT trap and
# on explicit interrupt. Idempotent: guards every step; never exits non-zero.
# ---------------------------------------------------------------------------
_cleanup_inflight() {
  # Kill in-flight agent process group (SIGTERM then SIGKILL after 3s).
  # Safety: never kill our own process group.
  if [ "${_INFLIGHT_AGENT_PGID:-0}" -ne 0 ] 2>/dev/null; then
    local pgid="$_INFLIGHT_AGENT_PGID"
    local our_pgid; our_pgid="$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')" || our_pgid="$$"
    _INFLIGHT_AGENT_PGID=0
    if [ "$pgid" != "$our_pgid" ] && [ "$pgid" != "$$" ]; then
      kill -- "-$pgid" 2>/dev/null || true
      # Brief grace period then escalate.
      local i=0
      while kill -0 -- "-$pgid" 2>/dev/null && [ "$i" -lt 3 ]; do
        sleep 1; i=$((i + 1))
      done
      kill -9 -- "-$pgid" 2>/dev/null || true
    fi
  fi
  # Prune stale worktree registrations then rm -rf in-flight slice dirs.
  if [ -n "${_INFLIGHT_CELL_UID:-}" ] && [ -n "${CORPUS_REPO_PATH:-}" ]; then
    git -C "$CORPUS_REPO_PATH" worktree prune >/dev/null 2>&1 || true
  fi
  local d
  for d in "${_INFLIGHT_CELL_DIRS[@]+"${_INFLIGHT_CELL_DIRS[@]}"}"; do
    [ -n "$d" ] && rm -rf "$d" 2>/dev/null || true
  done
  _INFLIGHT_CELL_DIRS=()
  _INFLIGHT_CELL_UID=""
}

# _on_exit — EXIT trap. Only runs cleanup when there IS an in-flight cell (i.e.
# an interrupted/crashed run). On the normal success path run_cell clears
# _INFLIGHT_CELL_UID before returning so this is a guaranteed no-op.
_on_exit() {
  _cleanup_inflight
}

# _on_interrupt — SIGINT/SIGTERM: run cleanup then re-raise so the shell exits
# with the right signal semantics (exit code 130 for INT, 143 for TERM).
_on_interrupt() {
  local sig="${1:-INT}"
  log "M4 trap: ${sig} received — cleaning up in-flight agent and worktrees"
  cell_log "interrupted" "{\"signal\":\"${sig}\"}" 2>/dev/null || true
  _cleanup_inflight
  trap - INT TERM EXIT
  kill -"$sig" "$$" 2>/dev/null || exit 1
}

# Install the trap handlers. They are installed here (top of script, after
# helpers are defined) and remain active for the entire process lifetime.
# shellcheck disable=SC2064  # intentional: trap strings are expanded at install time
trap '_on_exit' EXIT
trap '_on_interrupt INT'  INT
trap '_on_interrupt TERM' TERM

# ---------------------------------------------------------------------------
# spawn_agent_watchdog <model> <worktree> <brief>
#   Wraps the claude invocation with a pure-bash process-group watchdog.
#   * Spawns claude in its OWN process group (perl setpgid or setsid).
#   * Starts a background sleeper; if the sleeper fires first, kills the group.
#   * On deadline breach: writes a flag file (_AGENT_TIMEOUT_FLAG) so the
#     CALLER (spawn_agent) can detect timeout without a subshell variable.
#   * AGENT_TIMEOUT=0 disables the watchdog entirely (plain blocking call).
#   Called from spawn_agent; prints claude's JSON stdout on success.
#
# Timeout communication: spawn_agent_watchdog cannot set a global inside a
# $(…) command substitution (subshell). Instead it touches a flag file
# (_AGENT_TIMEOUT_FLAG) that spawn_agent checks after the call returns.
# ---------------------------------------------------------------------------
# Path to the per-invocation timeout flag file (set fresh each call).
_AGENT_TIMEOUT_FLAG=""
# Global set by spawn_agent (NOT in a subshell) after reading the flag.
AGENT_TIMED_OUT=0

spawn_agent_watchdog() {
  local model="$1" worktree="$2" brief="$3" extra_flags="${4:-}"
  # Reset flag (we are called from $(…) but the flag file persists on disk).
  [ -n "${_AGENT_TIMEOUT_FLAG:-}" ] && rm -f "$_AGENT_TIMEOUT_FLAG" 2>/dev/null || true

  if [ "${AGENT_TIMEOUT:-0}" -le 0 ] || [ "$DRY_RUN" -eq 1 ]; then
    # Watchdog disabled; delegate to the raw invocation.
    # shellcheck disable=SC2086
    cd "$worktree" && printf '%s' "$brief" | claude -p \
      --output-format=json \
      --model "$model" \
      $extra_flags \
      --add-dir "$worktree"
    return $?
  fi

  # Launch claude in its own process group so we can kill -<pgid> cleanly.
  # A temp file carries stdout from the agent (we cannot use a pipe here
  # because we need the pgid before we can wait; a subshell complicates the
  # pgid capture).
  local out_tmp; out_tmp="$(mktemp "${TMPDIR:-/tmp}/spawn-agent.XXXXXX")"
  # Start agent as a new process GROUP LEADER so kill -<pgid> kills only the
  # agent subtree (not the harness process).
  # On Linux: use setsid if available; fall back to perl setpgid.
  # On macOS: bash -c "set -m" does NOT create a new pgid in non-interactive
  #   mode, so we use "perl -e 'use POSIX; setpgid(0,0); exec @ARGV'" which
  #   is available on both macOS (system perl) and Linux.
  # The _pgid_launcher wrapper sets the new pgid and then execs the agent.
  # Choose the pgid isolation launcher.
  local use_setsid=0 use_perl=0
  command -v setsid >/dev/null 2>&1 && use_setsid=1
  command -v perl   >/dev/null 2>&1 && use_perl=1

  # The agent command as a bash -c invocation.
  # $1=$worktree $2=$brief $3=$model $4=$out_tmp $5=$extra_flags
  # extra_flags is passed as a single argument and word-split inside the
  # subshell (bash -c) deliberately — it carries zero or one flag token pair
  # like "--permission-mode acceptEdits" for the coder, or "" for the reviewer.
  local agent_cmd="cd \"\$1\" && printf '%s' \"\$2\" | claude -p \
    --output-format=json \
    --model \"\$3\" \
    \$5 \
    --add-dir \"\$1\" >\"\$4\" 2>/dev/null"

  if [ "$use_setsid" -eq 1 ]; then
    # Linux: setsid creates a new session → new pgid.
    setsid bash -c "$agent_cmd" -- "$worktree" "$brief" "$model" "$out_tmp" "$extra_flags" &
  elif [ "$use_perl" -eq 1 ]; then
    # macOS + Linux fallback: perl setpgid then exec bash.
    perl -e 'use POSIX; setpgid(0,0); exec @ARGV' -- \
      bash -c "$agent_cmd" -- "$worktree" "$brief" "$model" "$out_tmp" "$extra_flags" &
  else
    # No isolation: watchdog kills by PID, may miss deep descendants.
    bash -c "$agent_cmd" -- "$worktree" "$brief" "$model" "$out_tmp" "$extra_flags" &
  fi
  local agent_pid=$!
  # Capture the DISTINCT process group id of the background child.
  # Retry briefly: pgid may not be set instantly for the launched process.
  local pgid="" i=0
  while [ "$i" -lt 20 ]; do
    pgid="$(ps -o pgid= -p "$agent_pid" 2>/dev/null | tr -d ' ')" || pgid=""
    if [ -n "$pgid" ] && [ "$pgid" != "$$" ] && [ "$pgid" != "$(ps -o pgid= -p $$ | tr -d ' ')" ]; then
      break
    fi
    pgid=""
    sleep 0.1 2>/dev/null || true
    i=$((i + 1))
  done
  # Fall back: if we couldn't isolate the pgid, track by PID only. The kill
  # will still kill the agent but may not reach all descendants.
  [ -n "$pgid" ] || pgid="$agent_pid"
  _INFLIGHT_AGENT_PGID="$pgid"

  # Background sleeper: fires after AGENT_TIMEOUT seconds, sends SIGTERM to
  # the watchdog function's subshell which the main shell reaps. We use a
  # flag file rather than a signal so the pure-bash path is portable.
  local flag_tmp; flag_tmp="$(mktemp "${TMPDIR:-/tmp}/spawn-timeout.XXXXXX")"
  rm -f "$flag_tmp"  # will be re-created when sleeper fires
  ( sleep "$AGENT_TIMEOUT" && touch "$flag_tmp" ) &
  local sleeper_pid=$!

  # Poll: wait for either the agent (agent_pid gone) or the flag file.
  local rc=0
  while kill -0 "$agent_pid" 2>/dev/null; do
    if [ -f "$flag_tmp" ]; then
      # Deadline breached — kill the process group (safety: guard our own pgid).
      log "M4 watchdog: agent TIMED OUT after ${AGENT_TIMEOUT}s (pgid=${pgid}) — killing group"
      local our_pgid_w; our_pgid_w="$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')" || our_pgid_w="$$"
      if [ "$pgid" != "$our_pgid_w" ] && [ "$pgid" != "$$" ]; then
        kill -- "-$pgid" 2>/dev/null || true
        sleep 1
        kill -9 -- "-$pgid" 2>/dev/null || true
      else
        # Could not isolate pgid; kill by direct PID.
        kill "$agent_pid" 2>/dev/null || true
        sleep 1
        kill -9 "$agent_pid" 2>/dev/null || true
      fi
      wait "$agent_pid" 2>/dev/null || true
      # Signal timeout to the caller via a flag file (global won't survive subshell).
      [ -n "${_AGENT_TIMEOUT_FLAG:-}" ] && touch "$_AGENT_TIMEOUT_FLAG" 2>/dev/null || true
      _INFLIGHT_AGENT_PGID=0
      kill "$sleeper_pid" 2>/dev/null || true
      rm -f "$flag_tmp" "$out_tmp" 2>/dev/null || true
      return 1
    fi
    sleep 0.5 2>/dev/null || true
  done
  wait "$agent_pid" 2>/dev/null || rc=$?
  _INFLIGHT_AGENT_PGID=0

  # Kill the sleeper (it may or may not have fired).
  kill "$sleeper_pid" 2>/dev/null || true
  wait "$sleeper_pid" 2>/dev/null || true
  rm -f "$flag_tmp" 2>/dev/null || true

  if [ "$rc" -ne 0 ]; then
    rm -f "$out_tmp" 2>/dev/null || true
    return "$rc"
  fi
  cat "$out_tmp" 2>/dev/null || true
  rm -f "$out_tmp" 2>/dev/null || true
  return 0
}

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
# spawn_agent <model> <worktree> <brief> [out_dir]  ->  prints a token_sample JSON object
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
#
# When out_dir is provided the RAW claude JSON is also written to
# $out_dir/coder.raw.json (before the jq projection) so failed runs remain
# diagnosable from saved artifacts.
spawn_agent() {
  local model="$1" worktree="$2" brief="$3" out_dir="${4:-}"
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
  #
  # M4: spawn_agent_watchdog wraps the invocation with a per-process-group
  # watchdog. spawn_agent_watchdog is called inside $(…) so it cannot set a
  # global directly. Instead it writes a flag file (_AGENT_TIMEOUT_FLAG) on
  # timeout; we check the file after the call and set AGENT_TIMED_OUT here in
  # the parent shell where globals are preserved.
  local _timeout_flag; _timeout_flag="$(mktemp "${TMPDIR:-/tmp}/agent-toflag.XXXXXX")"
  rm -f "$_timeout_flag"  # watchdog touches it on timeout; absence = no timeout
  _AGENT_TIMEOUT_FLAG="$_timeout_flag"
  AGENT_TIMED_OUT=0
  # Coder gets --permission-mode acceptEdits (isolated throwaway worktree).
  raw="$(spawn_agent_watchdog "$model" "$worktree" "$brief" "--permission-mode acceptEdits")"
  if [ -f "$_timeout_flag" ]; then AGENT_TIMED_OUT=1; fi
  rm -f "$_timeout_flag" 2>/dev/null || true
  _AGENT_TIMEOUT_FLAG=""

  # Preserve the raw claude JSON alongside the token projection so failure runs
  # are diagnosable from saved artifacts (the projection discards everything
  # except the five usage fields, making silent failures undiagnosable).
  if [ -n "$out_dir" ]; then
    mkdir -p "$out_dir"
    printf '%s' "$raw" >"$out_dir/coder.raw.json" 2>/dev/null || true
  fi

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

# spawn_agent_raw <model> <worktree> <brief> [out_dir]  ->  prints the full raw claude JSON
#
# Like spawn_agent but WITHOUT --permission-mode acceptEdits (reviewer does not
# need to edit files) and returns the FULL result JSON, not the token_sample
# projection.  The caller is responsible for extracting the token_sample and
# the verdict from the raw JSON independently.
#
# Shares spawn_agent_watchdog with the coder: process-group isolation,
# _INFLIGHT_AGENT_PGID registration, timeout, and flag-file timeout signalling
# are all identical.  Only the extra_flags argument differs (empty string =
# no --permission-mode flag).
#
# When out_dir is provided the RAW claude JSON is also written to
# $out_dir/reviewer.raw.json before the caller applies any projection.
spawn_agent_raw() {
  local model="$1" worktree="$2" brief="$3" out_dir="${4:-}"
  local raw _wd_rc=0

  local _timeout_flag; _timeout_flag="$(mktemp "${TMPDIR:-/tmp}/agent-toflag.XXXXXX")"
  rm -f "$_timeout_flag"
  _AGENT_TIMEOUT_FLAG="$_timeout_flag"
  AGENT_TIMED_OUT=0
  # Reviewer does not edit files → no --permission-mode acceptEdits.
  raw="$(spawn_agent_watchdog "$model" "$worktree" "$brief" "")" || _wd_rc=$?
  if [ -f "$_timeout_flag" ]; then AGENT_TIMED_OUT=1; fi
  rm -f "$_timeout_flag" 2>/dev/null || true
  _AGENT_TIMEOUT_FLAG=""

  # Preserve the raw reviewer JSON for diagnosability (the verdict + full result
  # text would otherwise be lost once the caller projects only the token fields).
  if [ -n "$out_dir" ]; then
    mkdir -p "$out_dir"
    printf '%s' "$raw" >"$out_dir/reviewer.raw.json" 2>/dev/null || true
  fi

  if [ "$_wd_rc" -ne 0 ]; then
    # Propagate the watchdog's non-zero exit (timeout or agent failure) so the
    # caller can distinguish a timed-out/failed run from an empty-result run.
    return "$_wd_rc"
  fi
  # Emit the full raw JSON; the caller projects to token_sample and verdict.
  printf '%s' "$raw"
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

# verify_feature_absent <repo> <base_sha> <override:0|1> <path...> — the
# BASE-FIDELITY GATE. Asserts the feature does NOT already exist at base_sha.
# The check: declared paths must be ABSENT at base — if the file the plan
# claims to create already exists at base, the cell would measure a no-op.
#
# When <override> is 1 (operator-supplied base), the hard-fail is SUPPRESSED:
# the operator asserts base fidelity by construction (base = parent of earliest
# task commit), and the corpus features are modify-features whose paths
# legitimately pre-exist. Instead we log each pre-existing path as INFO and
# emit the operator-asserted message — no abort. The pick_base_sha hard-fail
# path (override=0) is unchanged.
#
# M1 uses path-existence as the absence signal (path-level harvest is the MVP,
# decision D1). A path that already exists at base is the pre-existence bug
# this gate fixes. Later milestones can tighten this to symbol/acceptance-
# signal absence; the seam is this function.
verify_feature_absent() {
  local repo="$1" base="$2" override="$3"; shift 3
  local p preexisting=""
  for p in "$@"; do
    # `git cat-file -e <sha>:<path>` exits 0 iff the path exists in that tree.
    if git -C "$repo" cat-file -e "${base}:${p}" 2>/dev/null; then
      preexisting="${preexisting}${preexisting:+ }${p}"
    fi
  done
  if [ -n "$preexisting" ]; then
    if [ "$override" -eq 1 ]; then
      # Override in effect: base fidelity is operator-asserted. Log, do not abort.
      log "base-fidelity: operator-asserted (override base ${base}); existence gate skipped for modify-feature corpus"
      log "base-fidelity: the following declared path(s) pre-exist at base (INFO — expected for modify-features): ${preexisting}"
    else
      err "base-fidelity gate FAILED: declared path(s) already exist at base_sha ${base}: ${preexisting}
       the feature pre-exists at base; this cell would measure a no-op. Refusing."
    fi
  else
    log "base-fidelity gate OK: no declared path pre-exists at base ${base}"
  fi
}

# lookup_base_override <plan> — check whether an operator-supplied base SHA
# exists for the given plan in BENCH_BASES. BENCH_BASES is a comma-separated
# list of "plan_id:sha" pairs (e.g. "659:fcb167a,668:274b6f6"). Prints the
# override SHA on stdout if found; prints nothing if absent. Validates the SHA
# is a real commit (git rev-parse --verify <sha>^{commit}) and err()s if not.
lookup_base_override() {
  local plan="$1" entry sha
  # Also accept a single-cell --base override (ARG_BASE), which has priority.
  if [ -n "$ARG_BASE" ]; then
    sha="$ARG_BASE"
    git -C "$CORPUS_REPO_PATH" rev-parse --verify "${sha}^{commit}" >/dev/null 2>&1 \
      || err "base override: --base '${sha}' is not a valid commit in ${CORPUS_REPO_PATH}"
    printf '%s' "$sha"
    return 0
  fi
  [ -n "$BENCH_BASES" ] || return 0
  # Parse "plan:sha,plan:sha,..." — accept comma or space separation.
  local bases_normalized; bases_normalized="$(printf '%s' "$BENCH_BASES" | tr ',' ' ')"
  for entry in $bases_normalized; do
    local eid; eid="${entry%%:*}"
    local esha; esha="${entry#*:}"
    if [ "$eid" = "$plan" ]; then
      git -C "$CORPUS_REPO_PATH" rev-parse --verify "${esha}^{commit}" >/dev/null 2>&1 \
        || err "base override: BENCH_BASES entry '${entry}' for plan ${plan} is not a valid commit in ${CORPUS_REPO_PATH}"
      printf '%s' "$esha"
      return 0
    fi
  done
  # No override for this plan.
  return 0
}

# ===========================================================================
# run_uid minting + config_hash  (D8 / run-record-schema §2)
# ===========================================================================
#
# mint_run_uid <plan> <arm> <rep> — harness-minted stable id. M1/M2 single-cell
# mode appends a timestamp for uniqueness; M3 mints a DETERMINISTIC id (no
# timestamp) so resume can recompute the same uid and find the prior record (see
# deterministic_run_uid). The cell honors an injected CELL_RUN_UID_OVERRIDE
# (set by the matrix driver) ahead of this fallback.
mint_run_uid() {
  printf 'bench-%s-%s-rep%s-%s' "$1" "$2" "$3" "$(date -u +%Y%m%dT%H%M%SZ)"
}

# deterministic_run_uid <plan> <arm> <rep> — the M3 stable cell id. NO
# timestamp: re-running the harness recomputes the identical uid so `bench show`
# can detect a prior completed/running record (resume/idempotency, §4). Retry
# suffixes (-retryN) are minted by the driver when a crashed run is aborted.
deterministic_run_uid() {
  printf 'm-%s-%s-r%s' "$1" "$2" "$3"
}

# config_hash_base — the arm-EXCLUDED frozen-config fingerprint. This is the
# paired-blocking key (§4): the 3 arms of a given (plan,rep) share an identical
# config_hash differing ONLY in arm, so they join as paired comparisons
# (run-record-schema §2: comparable iff config_hash identical except arm). It
# hashes the frozen nuisance vars — models, iteration cap, brief-template
# version, solver, budget ceiling — but NOT the arm. sha256, first 16 hex.
config_hash_base() {
  printf '%s|%s|%s|%s|%s|%s' \
    "$CODER_MODEL" "$REVIEWER_MODEL" "$ITER_CAP" \
    "$BRIEF_TEMPLATE_VERSION" "$SOLVER" "$CEILING" \
    | shasum -a 256 | awk '{print substr($1,1,16)}'
}

# config_hash <arm> — the GROUP BY key. The arm-excluded base PLUS the arm.
# Paired cells differ only in arm (D-HARNESS): config_hash(plan,rep,armA) and
# config_hash(plan,rep,armB) share the same base segment. sha256, first 16 hex.
config_hash() {
  local arm="$1"
  printf '%s|%s' "$(config_hash_base)" "$arm" \
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

COMMIT DISCIPLINE (load-bearing — the harness reads your committed work):
When you are done, you MUST commit everything you changed in this worktree
with a plain git commit:
  git add -A && git commit -m "${plan}: <one-line summary of the task>"
Both the conflict-detection instrument (which merges your committed tip) and
the actual-touch harvest (which diffs your committed range) read ONLY committed
state. Uncommitted work is invisible to the measurement. If you genuinely
changed nothing, do not fabricate a commit — a no-change task is a valid
outcome and the harness records zero touches for it.
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
  # Pass $out so spawn_agent writes coder.raw.json alongside coder.json —
  # the raw transcript is preserved for diagnosability (see Change 3).
  spawn_agent "$CODER_MODEL" "$worktree" \
    "$(coder_brief "$CELL_PLAN" "$CELL_PROBLEM")" "$out" >"$out/coder.json" 2>/dev/null \
    || printf '{"in":0,"out":0,"cache_in":0,"cache_out":0,"usd":0}' >"$out/coder.json"
  # M4 fix: route the reviewer through spawn_agent_raw (watchdog + process-group
  # isolation + _INFLIGHT_AGENT_PGID registration) instead of a raw `claude -p`.
  # spawn_agent_raw returns the FULL result JSON (needed for reviewer_verdict);
  # the token_sample projection is applied here, matching the coder's shape.
  # Pass $out so spawn_agent_raw writes reviewer.raw.json alongside reviewer.json.
  local reviewer_raw
  reviewer_raw="$(spawn_agent_raw "$REVIEWER_MODEL" "$worktree" \
    "$(reviewer_brief "$CELL_PLAN" "$CELL_PROBLEM")" "$out" 2>/dev/null || true)"
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
  # A slice that committed nothing (HEAD still == base) integrates trivially:
  # there is literally nothing to merge, so this is a clean no-op. Do NOT run a
  # git merge here — the previous code ran `git merge` in $CORPUS_REPO_PATH (the
  # corpus's OWN checkout), masked by `|| true`, which could mutate the corpus
  # working tree. Just return: zero committed work == clean integration.
  if [ -z "$slice_head" ] || [ "$slice_head" = "$CELL_BASE_SHA" ]; then
    log "fan-in: slice ${tasks_json} committed nothing (HEAD==base) -> clean no-op"
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

# commit_agent_work <worktree> <label> — commit any work the agent left
# UNCOMMITTED in the slice worktree. This is the authoritative commit step:
# agents proved unreliable at committing (a 45-cell campaign completed with 375
# declared touches and ZERO actual touches because agents left edits staged but
# not committed — the harness now owns the commit so harvest_slice always sees a
# committed range). The coder_brief still instructs the agent to commit as a
# backstop, but the harness commit is authoritative.
#
# Uses `git diff --cached --quiet && git diff --quiet` to detect "nothing to
# stage/commit" and skips the commit cleanly in that case (a genuinely-empty
# slice is valid — it yields zero actual touches, correctly). Safe under
# set -euo pipefail: the "nothing staged" guard prevents `git commit` from
# exiting non-zero on an empty index.
commit_agent_work() {
  local worktree="$1" label="$2"
  # Stage everything the agent touched (mirrors the coder_brief instruction).
  git -C "$worktree" add -A 2>/dev/null || true
  # Only commit if there is something in the index. `git diff --cached --quiet`
  # exits 0 when the index is empty (nothing staged); exit 1 = staged changes.
  if git -C "$worktree" diff --cached --quiet 2>/dev/null; then
    log "commit_agent_work: nothing staged in worktree ${worktree} — no commit (clean empty slice)"
    return 0
  fi
  git -C "$worktree" \
    -c user.email="bench-harness@planar.local" \
    -c user.name="bench-harness" \
    commit -m "$label" >/dev/null 2>&1 \
    && log "commit_agent_work: committed agent work in ${worktree} (label: ${label})" \
    || log "commit_agent_work: git commit failed in ${worktree} — continuing (harvest will record 0 touches)"
  return 0
}

# harvest_slice <worktree> <tasks_json> — per-task actual-touch harvest for one
# slice. `bench harvest` diffs the slice's COMMITTED range (base..committed-HEAD)
# and writes kind=actual rows. Range mode (not working-tree mode) is what makes
# harvest agree with fanin_conflict_check: both read ONLY committed state, so an
# agent that commits its work is recorded consistently by both instruments, and
# an agent that committed nothing yields zero actual touches here AND a clean
# no-op at fan-in (a real "task touched nothing" outcome, not a mis-record).
# Range mode also correctly includes created files that were committed (a
# working-tree `git diff` vs HEAD would miss them once committed). Per-task even
# for a grouped (multi-task) slice, so per-task touch precision/recall stays
# computable (the co-located tasks share the slice's committed diff, which is the
# documented grouped trade-off, decision D1).
harvest_slice() {
  local worktree="$1" tasks_json="$2" tid head_sha
  head_sha="$(git -C "$worktree" rev-parse HEAD 2>/dev/null || true)"
  # No commit (HEAD still == base, or unreadable): nothing was committed, so the
  # slice touched nothing. Skip the range harvest entirely — recording zero
  # actual touches is the correct, consistent outcome (matches fan-in's no-op).
  if [ -z "$head_sha" ] || [ "$head_sha" = "$CELL_BASE_SHA" ]; then
    log "harvest: slice ${tasks_json} committed nothing (HEAD==base) -> 0 actual touches"
    return 0
  fi
  for tid in $(printf '%s' "$tasks_json" | jq -r '.[]'); do
    pl bench harvest "$CELL_RUN_UID" --task "$tid" --worktree "$worktree" \
      --base "$CELL_BASE_SHA" --head "$head_sha" >&2 || true
  done
}

# run_slice <slice_tag> <tasks_json> — drive ONE serial slice end-to-end:
# worktree -> B-phase agents -> harness commit -> fan-in (events + conflict
# check) -> harvest.
# strict, grouped, and the eligibility serial-remainder all call this.
run_slice() {
  local tag="$1" tasks_json="$2" worktree out
  title "slice ${tag} tasks=${tasks_json} (serial)"
  worktree="$(slice_worktree "$tag")"
  out="$TRANSCRIPT_ROOT/$CELL_RUN_UID/$tag"
  run_agents "$worktree" "$out"
  # Harness commits whatever the agent left uncommitted. This must happen BEFORE
  # harvest_slice and fanin_conflict_check, both of which read committed state.
  commit_agent_work "$worktree" "${CELL_PLAN}: slice ${tag}"
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
      # Harness commits whatever the concurrent agent left uncommitted. Must run
      # before harvest_slice and fanin_conflict_check (both read committed state).
      commit_agent_work "$wt" "${CELL_PLAN}: slice ${tag}"
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
  local base_override; base_override="$(lookup_base_override "$plan")"
  if [ -n "$base_override" ]; then
    base_sha="$base_override"
    log "base_sha (operator-supplied override): ${base_sha}"
    if [ -n "$paths_list" ]; then
      # Gate softened: override base; existence check logs but does not abort.
      # shellcheck disable=SC2046
      verify_feature_absent "$CORPUS_REPO_PATH" "$base_sha" 1 $paths_list
    fi
  elif [ -n "$paths_list" ]; then
    # shellcheck disable=SC2046  # intentional word-split of the path list into argv
    base_sha="$(pick_base_sha "$CORPUS_REPO_PATH" $paths_list)"
    log "base_sha (parent of first touching commit): ${base_sha}"
    # shellcheck disable=SC2046
    verify_feature_absent "$CORPUS_REPO_PATH" "$base_sha" 0 $paths_list
  else
    base_sha="$(git -C "$CORPUS_REPO_PATH" rev-parse HEAD)"
    log "base_sha (no declared paths; using corpus HEAD): ${base_sha}"
  fi

  # --- cell identity. The matrix driver (M3) injects a DETERMINISTIC run_uid
  #     and the PAIRED config_hash via CELL_RUN_UID_OVERRIDE / CELL_CHASH_OVERRIDE
  #     so the 3 arms of a (plan,rep) share a hash differing only in arm and so
  #     resume can recompute the uid. Single-cell M1/M2 mode leaves them empty
  #     and falls back to the timestamped uid + locally-computed hash. ---
  run_uid="${CELL_RUN_UID_OVERRIDE:-$(mint_run_uid "$plan" "$arm" "$rep")}"
  chash="${CELL_CHASH_OVERRIDE:-$(config_hash "$arm")}"
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

  # M4: register the in-flight cell so the trap handler can clean up if we are
  # interrupted before completion. _INFLIGHT_CELL_DIRS tracks the worktrees this
  # cell owns; they are added as they are created below.
  _INFLIGHT_CELL_UID="$run_uid"
  _INFLIGHT_CELL_DIRS=()

  # M4: structured log — cell_start event.
  cell_log "cell_start" \
    "$(jq -nc --arg rep "$rep" --arg base "$base_sha" --argjson tasks "$tasks_json" \
       '{rep:$rep, base_sha:$base, tasks:$tasks}')" || true

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
  _INFLIGHT_CELL_DIRS+=("$INTEG_WORKTREE")

  # --- phase A: setup (reset + bench start + declared snapshot). Run against a
  #     throwaway scratch worktree off base_sha (the ritual resets it; the real
  #     per-slice work happens in the slice worktrees the dispatchers cut). ---
  title "phase A: setup"
  local scratch_wt="$WORKTREE_ROOT/$run_uid/_setup"
  rm -rf "$scratch_wt"
  git -C "$CORPUS_REPO_PATH" worktree add --detach "$scratch_wt" "$base_sha" >&2
  _INFLIGHT_CELL_DIRS+=("$scratch_wt")
  execute_phase setup "$scratch_wt" "$setup_args" >&2

  # --- phase B + fan-in: dispatch the arm's characteristic slice shape ---
  title "phase B: arm dispatch (${arm})"
  case "$arm" in
    strict)      dispatch_strict "$plan" "$tasks_json" ;;
    eligibility) dispatch_eligibility "$plan" ;;
    grouped)     dispatch_grouped "$plan" ;;
    *) err "run_cell: unknown arm '${arm}'" ;;
  esac

  # --- zero-touch guard: if ALL slices produced 0 actual touches emit a loud
  #     WARNING so no future run can silently look like success while carrying no
  #     RQ1 data. A 45-cell campaign once completed exit-0 with $165 spent and
  #     ZERO actual touches because agents left edits uncommitted; this guard
  #     makes that class of failure immediately visible in the run log. ---
  if [ -f "$PLANAR_DB" ]; then
    local _n_actual
    _n_actual="$(sqlite3 "$PLANAR_DB" \
      "select count(*) from run_touches t
       join runs r on r.id = t.run_id
       where r.run_uid = '${run_uid}' and t.kind = 'actual';" \
      2>/dev/null || printf '0')"
    if [ "${_n_actual:-0}" -eq 0 ]; then
      log "WARNING: cell ${run_uid} harvested 0 actual touches — agent produced no committed changes; RQ1 data for this cell is empty"
    else
      log "zero-touch guard: cell ${run_uid} has ${_n_actual} actual touch(es) — OK"
    fi
  fi

  # --- phase C: finish the run (harvest already happened per-slice). The run is
  #     joinable: declared touches (snapshotted at start) + per-task actual
  #     touches (harvested per slice) + the full event journal. ---
  title "phase C: finish"
  pl bench finish "$run_uid" --status completed >&2

  # M4: structured log — cell_complete event (disposition = completed).
  cell_log "cell_complete" '{"disposition":"completed"}' || true

  # M4: cell completed cleanly — clear in-flight state so the EXIT trap no-ops.
  _INFLIGHT_CELL_UID=""
  _INFLIGHT_CELL_DIRS=()

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
# 8. THE MATRIX LOOP  (M3: schedule + interleave + pairing + resume + ceiling)
# ===========================================================================
#
# The driver that wraps run_cell into the full confirmatory matrix:
# CORPUS_PLANS x ARMS x N_REPS, arm order RANDOMIZED per (rep,plan) so model
# drift over calendar time is randomized (§4), paired config_hash per (plan,rep)
# so the 3 arms join as paired comparisons (§4 blocking), DETERMINISTIC run_uids
# so a re-run resumes additively (skip completed, retry crashed), and a
# spend-ceiling stop (§9). --dry-run prints the whole interleaved schedule + the
# estimated total spend without dispatching — the operator pre-approval surface.
#
# M4 SEAMS (out of scope here, left clean): trap-based worktree cleanup,
# per-cell timeout/kill, and structured logging hook around dispatch_cell.

# corpus_plan_list — the resolved plan-list as whitespace-separated ids. Accepts
# comma- or space-separated CORPUS_PLANS; in single-cell-promoted matrix mode
# (operator passed --plan but --matrix) it falls back to that one plan.
corpus_plan_list() {
  if [ -n "$CORPUS_PLANS" ]; then
    printf '%s' "$CORPUS_PLANS" | tr ',' ' '
  else
    printf '%s' "$ARG_PLAN"
  fi
}

# shuffled_arms <seed> — the 3 arms in a DETERMINISTIC, seed-derived order. The
# seed is rep*1000+plan so the order is reproducible across re-runs (resume must
# recompute the identical schedule) yet varies per (rep,plan) — never all-of-one-
# arm-then-the-next (§4). A tiny Fisher-Yates over the fixed ARMS list using a
# splitmix-ish LCG seeded from the integer; pure bash 3.2, no external rng.
shuffled_arms() {
  local seed="$1"
  # Load the arms into an indexed array.
  local arr=() a
  for a in $ARMS; do arr[${#arr[@]}]="$a"; done
  local n=${#arr[@]} i j tmp
  # LCG state. Knuth MMIX constants, masked to 31 bits for bash arithmetic.
  local state=$(( (seed * 2654435761 + 1013904223) & 0x7fffffff ))
  for (( i = n - 1; i > 0; i-- )); do
    state=$(( (state * 1103515245 + 12345) & 0x7fffffff ))
    j=$(( state % (i + 1) ))
    tmp="${arr[$i]}"; arr[$i]="${arr[$j]}"; arr[$j]="$tmp"
  done
  printf '%s\n' "${arr[@]}"
}

# build_schedule — emit the full interleaved (rep, plan, arm) schedule, one
# "rep plan arm" triple per line. Outer loop is rep, then plan, then the
# seed-shuffled arms for that (rep,plan). Printing rep-major keeps the arms of a
# pair adjacent (so a pair completes before the next) while the per-pair shuffle
# randomizes which arm leads — the interleaving §4 asks for.
build_schedule() {
  local rep plan arm seed
  for rep in $(seq 1 "$N_REPS"); do
    for plan in $(corpus_plan_list); do
      seed=$(( rep * 1000 + plan ))
      while IFS= read -r arm; do
        printf '%s %s %s\n' "$rep" "$plan" "$arm"
      done <<EOF
$(shuffled_arms "$seed")
EOF
    done
  done
}

# cumulative_spend — the experiment-level spend ledger (§9). SUM of the `usd`
# field over EVERY token_sample payload across all NON-ABORTED runs in the
# isolated experiment DB. Modeled on metrics/tokens_per_plan.sql (same
# json_extract over run_events kind='token_sample'); aborted runs are excluded
# so a crashed-then-retried cell is not double-counted in the ledger. Prints a
# bare decimal on stdout. Returns 0.00 against an empty DB.
cumulative_spend() {
  # The DB file may not exist yet (first invocation, no cell run). Guard it so
  # the ledger query never aborts the driver under `set -e`.
  [ -f "$PLANAR_DB" ] || { printf '0.00'; return 0; }
  sqlite3 "$PLANAR_DB" "
    select coalesce(printf('%.4f', sum(json_extract(e.payload, '\$.usd'))), '0.00')
    from runs r
    join run_events e on e.run_id = r.id
    where e.kind = 'token_sample'
      and r.status <> 'aborted';" 2>/dev/null || printf '0.00'
}

# estimate_cell_cost — the next cell's projected USD cost. Once any completed
# cell exists, estimate from the observed per-cell average (cumulative spend /
# completed-cell count); before then, fall back to CELL_COST_DEFAULT. This feeds
# the ceiling pre-check and the dry-run total estimate.
estimate_cell_cost() {
  [ -f "$PLANAR_DB" ] || { printf '%s' "$CELL_COST_DEFAULT"; return 0; }
  local completed cum
  completed="$(sqlite3 "$PLANAR_DB" \
    "select count(*) from runs where status = 'completed';" 2>/dev/null || printf '0')"
  if [ -z "$completed" ] || [ "$completed" -eq 0 ]; then
    printf '%s' "$CELL_COST_DEFAULT"
    return 0
  fi
  cum="$(cumulative_spend)"
  awk -v c="$cum" -v n="$completed" 'BEGIN{ printf "%.4f", c / n }'
}

# usd_le <a> <b> — float a <= b (bash has no float compare). awk returns the
# boolean as an exit code: 0 (true) when a <= b.
usd_le() { awk -v a="$1" -v b="$2" 'BEGIN{ exit !(a <= b) }'; }
# usd_add <a> <b> — print a + b.
usd_add() { awk -v a="$1" -v b="$2" 'BEGIN{ printf "%.4f", a + b }'; }

# cell_status <run_uid> — the resume probe (§4 idempotency). Reads
# `bench show --json`:
#   absent     -> prints "absent"   (no record; dispatch fresh)
#   running    -> prints "running"  (crashed mid-cell; abort + retry)
#   completed  -> prints "completed" (skip, no double-count)
#   <other>    -> prints the raw status (aborted/error; dispatch fresh retry)
# `bench show` exits non-zero with "not found" for an absent run; that is the
# absent signal (distinguished from a real DB error by the run not existing).
cell_status() {
  local run_uid="$1" js
  if js="$(pl bench show "$run_uid" --json 2>/dev/null)"; then
    printf '%s' "$js" | jq -r '.status'
  else
    printf 'absent'
  fi
}

# resolve_cell_uid <plan> <arm> <rep> — the deterministic cell uid AFTER resume
# reconciliation. Returns (on stdout) the uid the driver should use for this
# cell, and (on stderr, via log) what it decided:
#   completed              -> prints "SKIP" — the caller skips dispatch entirely.
#   running (crashed)      -> aborts the stale record (bench finish --status
#                             aborted; left as audit, excluded from metrics) and
#                             mints a fresh <base>-retryN uid (next free N),
#                             UNLESS the retry cap is reached.
#   absent / aborted / err -> the base deterministic uid (fresh dispatch),
#                             UNLESS the retry cap is reached (SKIP + PERM_FAIL).
# Resume is strictly ADDITIVE: re-running never reuses a partial uid and never
# double-counts a completed one.
# M4: CELL_RETRY_CAP — once a cell has been aborted+retried that many times,
# prints "PERM_FAIL" so the caller logs loudly and skips the cell permanently.
resolve_cell_uid() {
  local plan="$1" arm="$2" rep="$3"
  local base status
  base="$(deterministic_run_uid "$plan" "$arm" "$rep")"
  status="$(cell_status "$base")"
  case "$status" in
    completed)
      log "resume: ${base} already completed -> SKIP (no double-count)"
      printf 'SKIP'
      return 0
      ;;
    running)
      # A crashed mid-cell run. Abort it (audit trail) and mint a fresh retry.
      log "resume: ${base} left RUNNING (crashed mid-cell) -> abort + retry"
      pl bench finish "$base" --status aborted >/dev/null 2>&1 || true
      # Find the next free -retryN suffix so repeated crashes never collide.
      local n=1 candidate
      while :; do
        candidate="${base}-retry${n}"
        [ "$(cell_status "$candidate")" = "absent" ] && break
        # A completed retry means this cell is actually done under the retry uid.
        if [ "$(cell_status "$candidate")" = "completed" ]; then
          log "resume: ${candidate} already completed -> SKIP"
          printf 'SKIP'
          return 0
        fi
        n=$(( n + 1 ))
      done
      # M4 retry cap: if we have already retried >= CELL_RETRY_CAP times, mark
      # the cell permanently failed and skip it.
      if [ "$((n - 1))" -ge "${CELL_RETRY_CAP:-2}" ]; then
        log "M4 retry cap: cell ${base} has crashed ${CELL_RETRY_CAP} time(s) -> PERM_FAIL (skip)"
        printf 'PERM_FAIL'
        return 0
      fi
      log "resume: minting fresh retry uid ${candidate}"
      printf '%s' "$candidate"
      ;;
    absent)
      printf '%s' "$base"
      ;;
    *)
      # aborted / error from a prior run: dispatch fresh under a retry uid so the
      # aborted record stays as audit and is not overwritten.
      log "resume: ${base} status=${status} -> fresh retry"
      local n=1 candidate
      while :; do
        candidate="${base}-retry${n}"
        [ "$(cell_status "$candidate")" = "absent" ] && break
        [ "$(cell_status "$candidate")" = "completed" ] && { printf 'SKIP'; return 0; }
        n=$(( n + 1 ))
      done
      # M4 retry cap: count prior aborted runs for this base uid.
      local prior_retries; prior_retries=$(( n - 1 ))
      if [ "$prior_retries" -ge "${CELL_RETRY_CAP:-2}" ]; then
        log "M4 retry cap: cell ${base} has ${prior_retries} aborted prior run(s) >= cap ${CELL_RETRY_CAP} -> PERM_FAIL"
        printf 'PERM_FAIL'
        return 0
      fi
      printf '%s' "$candidate"
      ;;
  esac
}

# dispatch_cell <plan> <arm> <rep> — wrap one run_cell with the resume probe,
# the paired config_hash injection, and the deterministic uid. Returns 0 on
# dispatch (or skip); the ceiling check happens in the caller BEFORE this.
# M4: handles PERM_FAIL (retry cap exhausted), timeout (run_cell crashes the
# cell but lets the matrix continue), and emits structured log records per
# cell disposition.
dispatch_cell() {
  local plan="$1" arm="$2" rep="$3"
  local uid chash
  chash="$(config_hash "$arm")"   # base-derived; paired across the (plan,rep) arms
  uid="$(resolve_cell_uid "$plan" "$arm" "$rep")"
  if [ "$uid" = "SKIP" ]; then
    log "cell (plan=${plan} arm=${arm} rep=${rep}) -> SKIPPED (already completed)"
    # M4: structured log for skipped cell (need temp CELL_* for cell_log context).
    local _save_uid="$CELL_RUN_UID" _save_plan="$CELL_PLAN" _save_arm="$CELL_ARM"
    CELL_RUN_UID="$(deterministic_run_uid "$plan" "$arm" "$rep")"
    CELL_PLAN="$plan"; CELL_ARM="$arm"; _INFLIGHT_CELL_UID="$CELL_RUN_UID"
    cell_log "cell_skipped" '{"disposition":"skipped","reason":"already_completed"}' || true
    CELL_RUN_UID="$_save_uid"; CELL_PLAN="$_save_plan"; CELL_ARM="$_save_arm"
    _INFLIGHT_CELL_UID=""
    return 0
  fi
  if [ "$uid" = "PERM_FAIL" ]; then
    log "M4 retry cap: cell (plan=${plan} arm=${arm} rep=${rep}) -> PERMANENTLY FAILED (retry cap=${CELL_RETRY_CAP}); skipping"
    # M4: structured log for permanently failed cell.
    local _base_uid; _base_uid="$(deterministic_run_uid "$plan" "$arm" "$rep")"
    local _save_uid="$CELL_RUN_UID" _save_plan="$CELL_PLAN" _save_arm="$CELL_ARM"
    CELL_RUN_UID="$_base_uid"; CELL_PLAN="$plan"; CELL_ARM="$arm"
    _INFLIGHT_CELL_UID="$_base_uid"
    cell_log "cell_perm_failed" \
      "$(jq -nc --argjson cap "${CELL_RETRY_CAP:-2}" \
         '{disposition:"perm_failed",reason:"retry_cap_exhausted",retry_cap:$cap}')" || true
    CELL_RUN_UID="$_save_uid"; CELL_PLAN="$_save_plan"; CELL_ARM="$_save_arm"
    _INFLIGHT_CELL_UID=""
    return 0
  fi
  # Inject the deterministic uid + paired hash; run_cell honors the overrides.
  CELL_RUN_UID_OVERRIDE="$uid"
  CELL_CHASH_OVERRIDE="$chash"
  ARG_REP="$rep"
  # M4: run_cell is allowed to crash (set -e would abort the script); wrap with
  # || true + check AGENT_TIMED_OUT to log the disposition and continue the matrix.
  local cell_rc=0
  run_cell "$plan" "$arm" "$rep" || cell_rc=$?
  if [ "$cell_rc" -ne 0 ]; then
    if [ "${AGENT_TIMED_OUT:-0}" -eq 1 ]; then
      log "M4: cell (plan=${plan} arm=${arm} rep=${rep} uid=${uid}) TIMED OUT — marked crashed; matrix continues"
      # Temporarily restore cell context so cell_log has the right uid/plan/arm.
      local _save_uid="$CELL_RUN_UID" _save_plan="$CELL_PLAN" _save_arm="$CELL_ARM"
      CELL_RUN_UID="$uid"; CELL_PLAN="$plan"; CELL_ARM="$arm"
      _INFLIGHT_CELL_UID="$uid"
      cell_log "cell_complete" \
        "$(jq -nc --argjson timeout "${AGENT_TIMEOUT:-600}" \
           '{disposition:"timed_out",timeout_secs:$timeout}')" || true
      CELL_RUN_UID="$_save_uid"; CELL_PLAN="$_save_plan"; CELL_ARM="$_save_arm"
      _INFLIGHT_CELL_UID=""
      AGENT_TIMED_OUT=0
    else
      log "M4: cell (plan=${plan} arm=${arm} rep=${rep} uid=${uid}) CRASHED (rc=${cell_rc}) — marked crashed; matrix continues"
      local _save_uid="$CELL_RUN_UID" _save_plan="$CELL_PLAN" _save_arm="$CELL_ARM"
      CELL_RUN_UID="$uid"; CELL_PLAN="$plan"; CELL_ARM="$arm"
      _INFLIGHT_CELL_UID="$uid"
      cell_log "cell_complete" \
        "$(jq -nc --argjson rc "$cell_rc" '{disposition:"crashed",exit_code:$rc}')" || true
      CELL_RUN_UID="$_save_uid"; CELL_PLAN="$_save_plan"; CELL_ARM="$_save_arm"
      _INFLIGHT_CELL_UID=""
    fi
  fi
  CELL_RUN_UID_OVERRIDE=""
  CELL_CHASH_OVERRIDE=""
}

# run_matrix — the M3 entry point. Build + print the schedule, then walk it
# cell-by-cell: ceiling pre-check (stop cleanly on breach), resume reconcile,
# dispatch. --dry-run prints the schedule, each cell's resolved run_uid +
# base_sha + paired config_hash, and the ESTIMATED total spend, dispatching
# nothing.
run_matrix() {
  local schedule rep plan arm uid chash
  schedule="$(build_schedule)"

  title "M3 matrix schedule (interleaved; arm order shuffled per rep,plan)"
  log "plans:   $(corpus_plan_list)"
  log "reps:    ${N_REPS}"
  log "arms:    ${ARMS}"
  log "ceiling: \$${CEILING} USD"
  printf '%s\n' "$schedule" | while IFS= read -r line; do
    [ -n "$line" ] || continue
    set -- $line; rep="$1"; plan="$2"; arm="$3"
    log "  rep=${rep} plan=${plan} arm=${arm}  config_hash=$(config_hash "$arm")  uid=$(deterministic_run_uid "$plan" "$arm" "$rep")"
  done

  # =====================================================================
  # DRY-RUN: print the full plan (schedule + per-plan base_sha + run_uids +
  # estimated total spend), dispatch nothing.
  # =====================================================================
  if [ "$DRY_RUN" -eq 1 ]; then
    title "DRY-RUN: resolved base_sha per plan"
    local p base paths ovr
    for p in $(corpus_plan_list); do
      ovr="$(lookup_base_override "$p" 2>/dev/null || true)"
      if [ -n "$ovr" ]; then
        base="$ovr"
        log "  plan ${p}: base_sha=${base}  (operator-supplied override)  paired_config_hash_base=$(config_hash_base)"
      else
        paths="$(declared_paths "$p")"
        if [ -n "$paths" ]; then
          # shellcheck disable=SC2046
          base="$(pick_base_sha "$CORPUS_REPO_PATH" $paths 2>/dev/null || printf 'UNRESOLVED')"
        else
          base="$(git -C "$CORPUS_REPO_PATH" rev-parse HEAD 2>/dev/null || printf 'UNRESOLVED')"
        fi
        log "  plan ${p}: base_sha=${base}  paired_config_hash_base=$(config_hash_base)"
      fi
    done

    title "DRY-RUN: cell run_uids (deterministic) + paired config_hash"
    printf '%s\n' "$schedule" | while IFS= read -r line; do
      [ -n "$line" ] || continue
      set -- $line; rep="$1"; plan="$2"; arm="$3"
      printf '  uid=%-22s arm=%-12s config_hash=%s\n' \
        "$(deterministic_run_uid "$plan" "$arm" "$rep")" "$arm" "$(config_hash "$arm")" >&2
    done

    local n_cells est_cell est_total cum
    n_cells="$(printf '%s\n' "$schedule" | grep -c . || printf '0')"
    est_cell="$(estimate_cell_cost)"
    cum="$(cumulative_spend)"
    est_total="$(awk -v n="$n_cells" -v c="$est_cell" 'BEGIN{ printf "%.2f", n * c }')"
    title "DRY-RUN: estimated spend"
    log "  cells:                 ${n_cells}"
    log "  est. per-cell cost:    \$${est_cell} USD"
    log "  est. total (new):      \$${est_total} USD"
    log "  already spent (ledger):\$${cum} USD"
    log "  ceiling:               \$${CEILING} USD"
    if usd_le "$(usd_add "$cum" "$est_total")" "$CEILING"; then
      log "  -> projected total within ceiling."
    else
      log "  -> WARNING: projected total EXCEEDS ceiling; the live run will stop early."
    fi
    title "DRY-RUN complete (no spawns, no mutations)"
    return 0
  fi

  # =====================================================================
  # LIVE matrix walk: per-cell ceiling pre-check, resume reconcile, dispatch.
  # =====================================================================
  title "M3 matrix: live walk"
  # NOTE: piping the schedule into a while-loop would subshell the loop body,
  # losing the dispatch side effects we need to observe between cells. Drive the
  # loop from a here-string so the body runs in the current shell.
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    set -- $line; rep="$1"; plan="$2"; arm="$3"

    # --- spend-ceiling pre-check (§9): stop BEFORE dispatching a cell that
    #     would breach. Completed runs are left intact; nothing new is spawned. ---
    local cum est projected
    cum="$(cumulative_spend)"
    est="$(estimate_cell_cost)"
    projected="$(usd_add "$cum" "$est")"
    if ! usd_le "$projected" "$CEILING"; then
      title "SPEND CEILING REACHED"
      log "cumulative \$${cum} + est. cell \$${est} = \$${projected} > ceiling \$${CEILING}"
      log "stopping cleanly: dispatching nothing new. completed runs are intact."
      log "ceiling reached — analyze what exists"
      return 0
    fi

    title "matrix cell rep=${rep} plan=${plan} arm=${arm} (spent \$${cum}, est cell \$${est})"
    dispatch_cell "$plan" "$arm" "$rep"
  done <<EOF
$schedule
EOF

  title "M3 matrix: schedule exhausted (all cells dispatched or skipped)"
  log "final cumulative spend: \$$(cumulative_spend) USD (ceiling \$${CEILING})"
}

# ===========================================================================
# argv + main
# ===========================================================================

usage() {
  sed -n '2,72p' "$0" | sed 's/^# \?//'
}

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --plan)     ARG_PLAN="$2"; shift 2 ;;
      --plans)    CORPUS_PLANS="$2"; shift 2 ;;
      --rep)      ARG_REP="$2"; shift 2 ;;
      --reps)     N_REPS="$2"; shift 2 ;;
      --tasks)    ARG_TASKS="$2"; shift 2 ;;
      --arm)      ARG_ARM="$2"; shift 2 ;;
      --solver)   SOLVER="$2"; shift 2 ;;
      --ceiling)  CEILING="$2"; shift 2 ;;
      --matrix)   ARG_MATRIX=1; shift ;;
      --dry-run)  DRY_RUN=1; shift ;;
      # Base overrides: --base <sha> for single-cell; --bases <plan:sha,...> for matrix.
      # Both mirror BENCH_BASES / ARG_BASE env conventions (see config section above).
      --base)     ARG_BASE="$2"; shift 2 ;;
      --bases)    BENCH_BASES="$2"; shift 2 ;;
      -h|--help)  usage; exit 0 ;;
      *) err "unknown flag: $1 (try --help)" ;;
    esac
  done
  if [ "$ARG_MATRIX" -eq 1 ]; then
    # Matrix mode sweeps a plan-list; it needs --plans OR a single --plan.
    [ -n "$CORPUS_PLANS" ] || [ -n "$ARG_PLAN" ] \
      || err "--matrix needs --plans <ids> (or a single --plan <id>) (try --help)"
    case "$N_REPS" in
      ''|*[!0-9]*) err "--reps must be a positive integer (got '$N_REPS')" ;;
    esac
  else
    [ -n "$ARG_PLAN" ] || err "--plan <id> is required (try --help)"
    case "$ARG_ARM" in
      strict|eligibility|grouped) : ;;
      *) err "--arm must be one of: strict | eligibility | grouped (got '$ARG_ARM')" ;;
    esac
  fi
}

main() {
  require_tool jq
  require_tool git
  require_tool shasum
  parse_args "$@"

  if [ "$ARG_MATRIX" -eq 1 ]; then require_tool sqlite3; fi

  # M4: resolve LOG_ROOT now that BENCH_HOME is stable.
  [ -n "$LOG_ROOT" ] || LOG_ROOT="$BENCH_HOME/cell-logs"

  title "bench-matrix: $([ "$ARG_MATRIX" -eq 1 ] && echo 'M3 matrix driver' || echo 'M2 arm-shape driver')"
  log "PLANAR_DB:          $PLANAR_DB"
  log "PLANAR_CONFIG_PATH: $PLANAR_CONFIG_PATH"
  log "corpus repo:        $CORPUS_REPO_PATH ($CORPUS_REPO_NAME)"
  log "coder/reviewer:     $CODER_MODEL / $REVIEWER_MODEL"
  log "iteration cap:      $ITER_CAP"
  log "agent timeout:      ${AGENT_TIMEOUT}s (0=disabled)"
  log "cell retry cap:     ${CELL_RETRY_CAP}"
  log "log root:           $LOG_ROOT"
  [ "$DRY_RUN" -eq 1 ] && log "MODE:               --dry-run (no spawns, no mutations)"

  if [ "$ARG_MATRIX" -eq 1 ]; then
    # M3: sweep CORPUS_PLANS x ARMS x N_REPS (interleaved, paired, resumable,
    # ceiling-bounded). run_matrix wraps run_cell in the outer loop.
    run_matrix
  else
    # SCOPE: single-cell mode drives exactly ONE cell of the requested arm, rep
    # as given (M1/M2). The matrix loop is --matrix.
    log "arm:                $ARG_ARM"
    [ "$ARG_ARM" = "grouped" ] && log "solver:             $SOLVER"
    run_cell "$ARG_PLAN" "$ARG_ARM" "$ARG_REP"
  fi
}

main "$@"
