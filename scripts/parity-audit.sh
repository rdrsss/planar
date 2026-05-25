#!/usr/bin/env bash
#
# parity-audit.sh — produce the raw behavioral diff between the archive Go
# binary and the current zig binary across the top-level verb surface.
#
# Phase 2 of plan 351 (parity-gap-tests). DATA COLLECTION ONLY: no fixes,
# no triage, no allowlist. The output drives Phase 2.5 (task 2364), which
# is where each gap is classified into one of four buckets (zig
# regression, Go bug, intentional divergence, cosmetic).
#
# Strategy
#
# 1. Ensure $PLANAR_GO_BIN exists; build it from $ARCHIVE/src via
#    `go -C $ARCHIVE/src build -o $PLANAR_GO_BIN ./cmd/planar` if absent.
#    Re-build on --rebuild-go.
# 2. Ensure ./bin/planar exists; run `make build` if absent.
# 3. Copy ~/.planar/planar.db to $AUDIT_DB (a tmp path). Export PLANAR_DB
#    pointing at the copy for every audited invocation — the live DB is
#    never written to.
# 4. Set up a "real-cwd" fixture: a tmp dir registered as a Planar
#    project bound to an assoc, so cwd-derive returns the assoc slug.
#    Uses a second tmp DB so the fixture state does not leak into the
#    main audit DB.
# 5. For each top-level verb (enumerated from `planar --help`) run a
#    small matrix of invocations against BOTH binaries with the same
#    DB+cwd, capture stdout/stderr/exit, diff them. Pre-skip verbs that
#    cannot be compared safely (init, ext sync, version).
# 6. Write scripts/parity-data/parity-gap-report.{json,md}.
#
# Output schema (json)
#
#   {
#     "generated_at": "<iso-8601>",
#     "go_bin": "<path>", "go_bin_sha": "<sha256>",
#     "zig_bin": "<path>", "zig_bin_sha": "<sha256>",
#     "audit_db": "<path>", "cwd_fixture_db": "<path>", "cwd_fixture_dir": "<path>",
#     "summary": {
#       "verbs": <int>, "invocations": <int>,
#       "gaps": <int>, "no_diff": <int>, "errors": <int>, "skipped": <int>
#     },
#     "skipped": [ { "verb": "...", "reason": "..." } ],
#     "results": [
#       { "verb": "<verb>", "invocation": "<label>", "args": [...],
#         "cwd": "<dir-or-empty>",
#         "go":  { "exit": <int>, "stdout_bytes": <int>, "stderr_bytes": <int>,
#                  "stdout_sha": "<sha>", "stderr_sha": "<sha>",
#                  "timed_out": <bool> },
#         "zig": { ... same shape ... },
#         "diff": { "size_bytes": <int>, "unified": "<diff-or-empty>" }
#       }, ...
#     ]
#   }
#
# Markdown report has a Summary at the top, then a "Gaps (by diff size,
# descending)" table, then per-gap unified diffs, then a "No-diff" table.
# Skipped verbs and errors are listed in their own sections.
#
# Constraints honored
# - Read-only against ~/.planar/planar.db (script copies it).
# - Idempotent — re-running on the same inputs reproduces the same output
#   modulo the embedded timestamp.
# - Pure bash 3.2 (no mapfile, no associative arrays).
# - No root, no sudo.
# - Exits 0 even when gaps exist (emitting gaps is the script's job).
#   Non-zero exit is reserved for tooling failures.
#
# Usage
#   scripts/parity-audit.sh
#   scripts/parity-audit.sh --rebuild-go      # force re-build of the Go binary
#   scripts/parity-audit.sh --verb tree       # restrict to one verb (debug aid)
#   scripts/parity-audit.sh --timeout 20      # per-invocation timeout seconds (default 10)
#   scripts/parity-audit.sh --help

set -euo pipefail

# ---------- defaults ----------

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$REPO_ROOT/scripts/parity-data"

PLANAR_GO_BIN="${PLANAR_GO_BIN:-$HOME/.planar-archive/bin/planar-go}"
ARCHIVE="${ARCHIVE:-/Users/mn/projects/github/rdrsss/planar-go-archive}"
ZIG_BIN="${ZIG_BIN:-$REPO_ROOT/bin/planar}"

SOURCE_DB="${SOURCE_DB:-$HOME/.planar/planar.db}"

REBUILD_GO=0
PER_VERB=""
TIMEOUT_SECS=10

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rebuild-go) REBUILD_GO=1; shift ;;
    --verb)       PER_VERB="$2"; shift 2 ;;
    --timeout)    TIMEOUT_SECS="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,/^set -euo/p' "$0" | sed 's/^# \?//;/^set -euo/d'
      exit 0 ;;
    *) echo "parity-audit: unknown flag: $1" >&2; exit 64 ;;
  esac
done

# ---------- helpers ----------

log()   { printf '  %s\n' "$*" >&2; }
title() { printf '\n==> %s\n' "$*" >&2; }
err()   { printf 'parity-audit: %s\n' "$*" >&2; exit 1; }

# Cross-platform sha256 of a file.
sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

# Cross-platform sha256 of stdin.
sha256_stdin() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  else
    sha256sum | awk '{print $1}'
  fi
}

# JSON string escape (bash 3.2). Newline → \n, tab → \t, CR → \r,
# backslash → \\, double-quote → \", control chars get a \u00xx escape
# (rough — sufficient for stdout/stderr blobs we attach for context).
json_escape() {
  local s="$1"
  # Use awk to emit a properly escaped JSON string body (no quotes).
  awk 'BEGIN { RS = "\1"; ORS = "" }
  {
    n = length($0)
    for (i = 1; i <= n; i++) {
      c = substr($0, i, 1)
      o = sprintf("%d", 0)
      # Get integer codepoint:
      "printf %d \"'\\''" c "\"" | getline o
      close("printf %d \"'\\''" c "\"")
      if      (c == "\\") printf "\\\\"
      else if (c == "\"") printf "\\\""
      else if (c == "\n") printf "\\n"
      else if (c == "\r") printf "\\r"
      else if (c == "\t") printf "\\t"
      else if (o + 0 < 32) printf "\\u%04x", o + 0
      else printf "%s", c
    }
  }' <<< "$s"
}

# Faster (and saner) JSON-string escape via python3 (always present on
# macOS/Linux dev machines). Falls back to a sed-pipe if python3 is
# missing.
json_escape_file() {
  local path="$1"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$path" <<'PY'
import json, sys
with open(sys.argv[1], 'rb') as f:
    data = f.read()
# Decode best-effort; replace undecodable bytes so we still emit valid JSON.
text = data.decode('utf-8', errors='replace')
sys.stdout.write(json.dumps(text))
PY
  else
    # Crude fallback: wrap and pray. Avoid binary blobs in this path.
    printf '"'
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e ':a;N;$!ba;s/\n/\\n/g' "$path"
    printf '"'
  fi
}

# Run a binary with a wall-clock timeout. Writes stdout to $3, stderr to
# $4. Echoes the exit code on stdout. Echoes "TIMED_OUT" via the
# .timedout sidecar file. We avoid the perl/timeout host dependency by
# spawning a watchdog subshell.
run_with_timeout() {
  local secs="$1" stdout_path="$2" stderr_path="$3"
  shift 3
  # Remaining args are the command + args.
  : > "$stdout_path"
  : > "$stderr_path"
  rm -f "$stdout_path.timedout"

  # Run the command in the background and capture its pid.
  ( exec "$@" ) >"$stdout_path" 2>"$stderr_path" &
  local cmd_pid=$!

  # Watchdog: if cmd_pid still exists after $secs, kill it.
  (
    local waited=0
    while kill -0 "$cmd_pid" 2>/dev/null; do
      if [[ "$waited" -ge "$secs" ]]; then
        touch "$stdout_path.timedout"
        kill -TERM "$cmd_pid" 2>/dev/null || true
        sleep 1
        kill -KILL "$cmd_pid" 2>/dev/null || true
        exit 0
      fi
      sleep 1
      waited=$((waited + 1))
    done
  ) &
  local wd_pid=$!

  local exit_code=0
  wait "$cmd_pid" 2>/dev/null || exit_code=$?
  kill "$wd_pid" 2>/dev/null || true
  wait "$wd_pid" 2>/dev/null || true

  echo "$exit_code"
}

# Normalize output for diffing: strip ANSI escape codes, replace the
# host-specific audit DB path with a stable token, and replace the cwd
# fixture path likewise. Volatile content (build SHAs, timestamps) is
# not normalized — those gaps are surfaced as cosmetic in Phase 2.5.
normalize_output() {
  local in_path="$1" out_path="$2"
  sed \
    -e 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
    -e "s#${AUDIT_DB}#<AUDIT_DB>#g" \
    -e "s#${CWD_DB}#<CWD_DB>#g" \
    -e "s#${CWD_DIR}#<CWD_DIR>#g" \
    -e "s#${PLANAR_GO_BIN}#<GO_BIN>#g" \
    -e "s#${ZIG_BIN}#<ZIG_BIN>#g" \
    "$in_path" > "$out_path"
}

# ---------- preflight ----------

title "parity-audit preflight"
log "repo root        = $REPO_ROOT"
log "go bin path      = $PLANAR_GO_BIN"
log "zig bin path     = $ZIG_BIN"
log "source db        = $SOURCE_DB"
log "archive          = $ARCHIVE"
log "rebuild-go       = $REBUILD_GO"
log "per-verb filter  = ${PER_VERB:-<all>}"
log "timeout          = ${TIMEOUT_SECS}s"

[[ -f "$SOURCE_DB" ]] || err "source db not found: $SOURCE_DB (run 'planar init' first?)"

# Ensure Go binary exists.
if [[ "$REBUILD_GO" -eq 1 || ! -x "$PLANAR_GO_BIN" ]]; then
  title "building Go archive binary"
  [[ -d "$ARCHIVE/src" ]] || err "archive missing: $ARCHIVE/src"
  command -v go >/dev/null 2>&1 || err "go toolchain not found on PATH"
  mkdir -p "$(dirname "$PLANAR_GO_BIN")"
  ( cd "$ARCHIVE/src" && go build -o "$PLANAR_GO_BIN" ./cmd/planar )
  log "built: $PLANAR_GO_BIN"
else
  log "using cached Go binary: $PLANAR_GO_BIN"
fi

[[ -x "$PLANAR_GO_BIN" ]] || err "Go binary not executable: $PLANAR_GO_BIN"

# Ensure zig binary exists.
if [[ ! -x "$ZIG_BIN" ]]; then
  title "building zig binary"
  ( cd "$REPO_ROOT" && make build )
fi
[[ -x "$ZIG_BIN" ]] || err "zig binary not built: $ZIG_BIN"

# ---------- audit DB + cwd fixture ----------

title "setting up audit DBs"

TMP_ROOT="$(mktemp -d -t parity-audit.XXXXXX)"
AUDIT_DB="$TMP_ROOT/audit.db"
CWD_DB="$TMP_ROOT/cwd.db"
CWD_DIR="$TMP_ROOT/cwd-project"

cp "$SOURCE_DB" "$AUDIT_DB"
log "audit db (copy of $SOURCE_DB): $AUDIT_DB"

# Fresh DB for the real-cwd fixture so its writes don't pollute audit.db.
# We use the zig binary's `init` to migrate the DB schema; the Go binary
# can open the same schema for reads. The fixture state lives outside
# the main audit so we keep determinism.
mkdir -p "$CWD_DIR"
PLANAR_DB="$CWD_DB" "$ZIG_BIN" init --allow-no-repo --name parity-audit-fixture >/dev/null 2>&1 \
  || err "zig init failed for cwd fixture"
PLANAR_DB="$CWD_DB" "$ZIG_BIN" assoc create parity-audit-fixture --kind project >/dev/null 2>&1 \
  || log "warn: assoc create returned non-zero (idempotent? continuing)"
# The `init --allow-no-repo` writes a project row pointing at the cwd
# from which init was invoked. We invoked it from this script's cwd, not
# $CWD_DIR. Re-init from inside $CWD_DIR via a subshell with cd:
( cd "$CWD_DIR" && PLANAR_DB="$CWD_DB" "$ZIG_BIN" init --allow-no-repo --name cwd-project >/dev/null 2>&1 ) \
  || log "warn: re-init in cwd-dir returned non-zero (may already be registered)"
( cd "$CWD_DIR" && PLANAR_DB="$CWD_DB" "$ZIG_BIN" assoc add parity-audit-fixture "$CWD_DIR" >/dev/null 2>&1 ) \
  || log "warn: assoc add returned non-zero (may already be added)"

log "cwd-fixture dir: $CWD_DIR  db: $CWD_DB"

# ---------- enumerate verbs ----------

title "enumerating verbs"

# Same parse as scripts/cli-surface-check.sh.
ALL_VERBS=$("$ZIG_BIN" --help 2>&1 \
  | awk '/^  [a-z]/ { print $1 }' \
  | grep -v '^planar$' \
  | sort -u)

# Pre-skipped verbs and their rationale. Keep this list tight — every
# entry must have a why. Phase 2.5 picks these up from the skipped[] in
# the JSON output.
#
# Format: <verb>|<reason>
PRESKIP_LIST="
init|init mutates the DB by registering the cwd as a project; not safe in the audit DB
version|build SHAs and zig-runtime strings always diverge; surfaces nothing useful pre-triage
completion|emits multi-kilobyte shell scripts that diverge in trivial ways across shells
sync|hits live external systems (Jira, GitHub) — network-dependent, not deterministic
"

is_preskipped() {
  local v="$1" line
  while IFS='|' read -r vn reason; do
    [[ -z "$vn" ]] && continue
    if [[ "$v" == "$vn" ]]; then
      echo "$reason"
      return 0
    fi
  done <<< "$PRESKIP_LIST"
  return 1
}

# Build the final list, honoring --verb filter.
VERBS=""
while IFS= read -r v; do
  [[ -z "$v" ]] && continue
  if [[ -n "$PER_VERB" && "$v" != "$PER_VERB" ]]; then continue; fi
  VERBS="${VERBS}${v}"$'\n'
done <<< "$ALL_VERBS"

log "verb count: $(printf '%s' "$VERBS" | grep -c .)"

# ---------- invocation matrix ----------

# Each verb gets these invocations. The label is used as the key in the
# diff index. Args are space-separated; if you need spaces inside an arg
# (we don't, yet), use a different encoding. CWD column is the
# working-directory to invoke from; empty = no chdir.
#
# Format: <invocation-label>|<cwd>|<arg1 arg2 ...>
#
# `help` and `flag-h` overlap with bare `--help`/`-h` and surface the
# same gap from different entry points — Phase 2.5 may collapse them.
INVOCATIONS="
help|.|--help
no-args|.|
json|.|--json
real-cwd|cwd|
real-cwd-json|cwd|--json
"

# Targeted invocations for the question-233 pre-seed verbs. These don't
# fit the generic matrix because they exercise specific argument shapes
# the orchestrator reported missing or inconsistent. We run them as
# extra rows after the generic matrix completes so Phase 2.5 sees the
# exact-shape gap, not just the verb-level surface area.
#
# Format: <verb>|<invocation-label>|<cwd>|<arg1 arg2 ...>
PRESEED_INVOCATIONS="
plan|q233-plan-next|.|next 351
agent|q233-agent-top|.|
agent|q233-agent-ps|.|ps
test-spec|q233-status-plan-flag|.|status --plan 351
test-spec|q233-status-positional|.|status 351
question|q233-add-plan-flag|.|add --plan 351 --title parity-probe --body x
task|q233-add-editor-false|.|add --plan 351 --title parity-probe --next-action x --editor=false
task|q233-add-no-editor|.|add --plan 351 --title parity-probe --next-action x --no-editor
"

# ---------- run matrix ----------

title "running matrix"

WORK="$TMP_ROOT/work"
mkdir -p "$WORK"

# Result-record sink. One line per record, pipe-separated, fields:
#   verb|invocation|cwd_label|args|
#   go_exit|go_stdout_bytes|go_stderr_bytes|go_stdout_sha|go_stderr_sha|go_timed_out|
#   zig_exit|zig_stdout_bytes|zig_stderr_bytes|zig_stdout_sha|zig_stderr_sha|zig_timed_out|
#   diff_bytes|diff_path|status
#
# status is one of: no_diff | gap | error
RECORDS="$WORK/records.txt"
: > "$RECORDS"

# We also keep per-invocation raw outputs under $WORK/raw/ for inclusion
# in the markdown report (the JSON only references shas + byte counts).
mkdir -p "$WORK/raw"
mkdir -p "$WORK/norm"
mkdir -p "$WORK/diffs"

verb_count=0
inv_count=0
gap_count=0
no_diff_count=0
error_count=0

# Emit the per-invocation runner shim (sourced argv → timeout-wrapped
# subprocess → exit code on stdout). Created before run_one is called.
RUNONE="$REPO_ROOT/scripts/parity-audit.sh.runone"
cat > "$RUNONE" <<'EOSH'
#!/usr/bin/env bash
# Internal helper for parity-audit.sh: run "$@" with timeout, dump
# stdout/stderr to provided paths, echo exit code.
set -uo pipefail
secs="$1"; stdout_path="$2"; stderr_path="$3"
shift 3
: > "$stdout_path"; : > "$stderr_path"
rm -f "$stdout_path.timedout"
( exec "$@" ) >"$stdout_path" 2>"$stderr_path" &
cmd_pid=$!
(
  waited=0
  while kill -0 "$cmd_pid" 2>/dev/null; do
    if [[ "$waited" -ge "$secs" ]]; then
      touch "$stdout_path.timedout"
      kill -TERM "$cmd_pid" 2>/dev/null || true
      sleep 1
      kill -KILL "$cmd_pid" 2>/dev/null || true
      exit 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
) &
wd_pid=$!
ec=0
wait "$cmd_pid" 2>/dev/null || ec=$?
kill "$wd_pid" 2>/dev/null || true
wait "$wd_pid" 2>/dev/null || true
echo "$ec"
EOSH
chmod +x "$RUNONE"

run_one() {
  local verb="$1" inv_label="$2" cwd_label="$3" args_str="$4"

  local cwd="$REPO_ROOT"
  local cwd_label_emit=""
  local db_for_run="$AUDIT_DB"
  if [[ "$cwd_label" == "cwd" ]]; then
    cwd="$CWD_DIR"
    cwd_label_emit="$CWD_DIR"
    db_for_run="$CWD_DB"
  fi

  local slug="${verb}__${inv_label}"
  local go_out="$WORK/raw/${slug}.go.out"
  local go_err="$WORK/raw/${slug}.go.err"
  local zig_out="$WORK/raw/${slug}.zig.out"
  local zig_err="$WORK/raw/${slug}.zig.err"

  # Build the argv. For top-level verbs that are multi-word (e.g.
  # 'test-spec status'), the verb stays one shell token because we
  # enumerated them as single tokens from --help. The args_str carries
  # any subcommand or flag. We split args_str on whitespace via $IFS
  # word-splitting; empty is handled by the "no extra args" branch
  # below (bash 3.2 'set -u' treats empty-array expansion as unbound).
  local go_exit go_timed_out zig_exit zig_timed_out
  if [[ -n "$args_str" ]]; then
    # shellcheck disable=SC2086
    go_exit=$( cd "$cwd" && PLANAR_DB="$db_for_run" \
      PLANAR_HOME="$TMP_ROOT/planar-home-go" \
      HOME="$TMP_ROOT/home-go" \
      "$RUNONE" "$TIMEOUT_SECS" "$go_out" "$go_err" \
      "$PLANAR_GO_BIN" "$verb" $args_str 2>/dev/null ) || go_exit="?"
  else
    go_exit=$( cd "$cwd" && PLANAR_DB="$db_for_run" \
      PLANAR_HOME="$TMP_ROOT/planar-home-go" \
      HOME="$TMP_ROOT/home-go" \
      "$RUNONE" "$TIMEOUT_SECS" "$go_out" "$go_err" \
      "$PLANAR_GO_BIN" "$verb" 2>/dev/null ) || go_exit="?"
  fi
  go_timed_out="false"
  [[ -f "$go_out.timedout" ]] && go_timed_out="true"

  if [[ -n "$args_str" ]]; then
    # shellcheck disable=SC2086
    zig_exit=$( cd "$cwd" && PLANAR_DB="$db_for_run" \
      PLANAR_HOME="$TMP_ROOT/planar-home-zig" \
      HOME="$TMP_ROOT/home-zig" \
      "$RUNONE" "$TIMEOUT_SECS" "$zig_out" "$zig_err" \
      "$ZIG_BIN" "$verb" $args_str 2>/dev/null ) || zig_exit="?"
  else
    zig_exit=$( cd "$cwd" && PLANAR_DB="$db_for_run" \
      PLANAR_HOME="$TMP_ROOT/planar-home-zig" \
      HOME="$TMP_ROOT/home-zig" \
      "$RUNONE" "$TIMEOUT_SECS" "$zig_out" "$zig_err" \
      "$ZIG_BIN" "$verb" 2>/dev/null ) || zig_exit="?"
  fi
  zig_timed_out="false"
  [[ -f "$zig_out.timedout" ]] && zig_timed_out="true"

  # Normalize then diff.
  local go_norm="$WORK/norm/${slug}.go.out"
  local zig_norm="$WORK/norm/${slug}.zig.out"
  normalize_output "$go_out" "$go_norm"
  normalize_output "$zig_out" "$zig_norm"
  local go_err_norm="$WORK/norm/${slug}.go.err"
  local zig_err_norm="$WORK/norm/${slug}.zig.err"
  normalize_output "$go_err" "$go_err_norm"
  normalize_output "$zig_err" "$zig_err_norm"

  local diff_path="$WORK/diffs/${slug}.diff"
  {
    # stdout diff
    if ! diff -u "$go_norm" "$zig_norm" 2>/dev/null; then :; fi
    # stderr diff (suffix-tagged so the report shows which stream)
    if ! diff -u --label "go.stderr" --label "zig.stderr" "$go_err_norm" "$zig_err_norm" 2>/dev/null; then :; fi
    # exit-code diff sentinel
    if [[ "$go_exit" != "$zig_exit" ]]; then
      printf -- '--- exit\n+++ exit\n-go=%s\n+zig=%s\n' "$go_exit" "$zig_exit"
    fi
  } > "$diff_path" 2>/dev/null || true

  local diff_bytes
  diff_bytes=$(wc -c < "$diff_path" | tr -d ' ')

  local go_stdout_bytes go_stderr_bytes zig_stdout_bytes zig_stderr_bytes
  go_stdout_bytes=$(wc -c < "$go_out" | tr -d ' ')
  go_stderr_bytes=$(wc -c < "$go_err" | tr -d ' ')
  zig_stdout_bytes=$(wc -c < "$zig_out" | tr -d ' ')
  zig_stderr_bytes=$(wc -c < "$zig_err" | tr -d ' ')

  local go_stdout_sha go_stderr_sha zig_stdout_sha zig_stderr_sha
  go_stdout_sha=$(sha256_file "$go_out")
  go_stderr_sha=$(sha256_file "$go_err")
  zig_stdout_sha=$(sha256_file "$zig_out")
  zig_stderr_sha=$(sha256_file "$zig_err")

  local status="no_diff"
  if [[ "$diff_bytes" -gt 0 ]]; then status="gap"; fi
  if [[ "$go_exit" == "?" || "$zig_exit" == "?" ]]; then status="error"; fi
  if [[ "$go_timed_out" == "true" || "$zig_timed_out" == "true" ]]; then status="error"; fi

  printf '%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n' \
    "$verb" "$inv_label" "$cwd_label_emit" "${args_str}" \
    "$go_exit" "$go_stdout_bytes" "$go_stderr_bytes" "$go_stdout_sha" "$go_stderr_sha" "$go_timed_out" \
    "$zig_exit" "$zig_stdout_bytes" "$zig_stderr_bytes" "$zig_stdout_sha" "$zig_stderr_sha" "$zig_timed_out" \
    "$diff_bytes" "$diff_path" "$status" \
    >> "$RECORDS"

  inv_count=$((inv_count + 1))
  case "$status" in
    gap)     gap_count=$((gap_count + 1)) ;;
    error)   error_count=$((error_count + 1)) ;;
    no_diff) no_diff_count=$((no_diff_count + 1)) ;;
  esac

  log "  ${verb} :: ${inv_label} → status=${status} diff_bytes=${diff_bytes}"
}

# Track skipped verbs (for the JSON skipped[] block).
SKIPPED="$WORK/skipped.txt"
: > "$SKIPPED"

while IFS= read -r v; do
  [[ -z "$v" ]] && continue
  verb_count=$((verb_count + 1))

  if reason=$(is_preskipped "$v"); then
    printf '%s|%s\n' "$v" "$reason" >> "$SKIPPED"
    log "skip ${v}: ${reason}"
    continue
  fi

  while IFS='|' read -r inv_label cwd_label args_str; do
    [[ -z "$inv_label" ]] && continue
    run_one "$v" "$inv_label" "$cwd_label" "$args_str"
  done <<< "$INVOCATIONS"
done <<< "$VERBS"

# Run the question-233 pre-seed invocations. These exercise specific
# arg shapes that the orchestrator flagged as missing/inconsistent in
# the zig binary. They surface as gaps even when the generic matrix
# would not have hit them (e.g. `agent ps` requires `agent` to exist
# as a top-level verb at all).
if [[ -z "$PER_VERB" ]]; then
  title "running question-233 pre-seed invocations"
  while IFS='|' read -r vp inv_label cwd_label args_str; do
    [[ -z "$vp" ]] && continue
    run_one "$vp" "$inv_label" "$cwd_label" "$args_str"
  done <<< "$PRESEED_INVOCATIONS"
fi

# Clean up the helper script before exit.
rm -f "$RUNONE"

# ---------- emit JSON ----------

title "writing JSON report"

JSON_OUT="$OUT_DIR/parity-gap-report.json"
mkdir -p "$OUT_DIR"

go_bin_sha=$(sha256_file "$PLANAR_GO_BIN")
zig_bin_sha=$(sha256_file "$ZIG_BIN")
generated_at=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
skipped_count=$(grep -c . "$SKIPPED" || true)

# Build JSON in pieces. We pipe records through python3 for safe
# string-escaping of diff bodies.
python3 - "$RECORDS" "$SKIPPED" "$JSON_OUT" \
  "$PLANAR_GO_BIN" "$go_bin_sha" \
  "$ZIG_BIN" "$zig_bin_sha" \
  "$AUDIT_DB" "$CWD_DB" "$CWD_DIR" \
  "$generated_at" \
  "$verb_count" "$inv_count" "$gap_count" "$no_diff_count" "$error_count" "$skipped_count" \
<<'PY'
import json, sys, os

(_, records_path, skipped_path, out_path,
 go_bin, go_sha, zig_bin, zig_sha,
 audit_db, cwd_db, cwd_dir, generated_at,
 verbs, invocations, gaps, no_diff, errors, skipped_n) = sys.argv

results = []
with open(records_path) as f:
    for line in f:
        line = line.rstrip("\n")
        if not line:
            continue
        parts = line.split("|")
        # See RECORDS schema in parity-audit.sh.
        (verb, inv, cwd, args,
         g_exit, g_so_b, g_se_b, g_so_sha, g_se_sha, g_to,
         z_exit, z_so_b, z_se_b, z_so_sha, z_se_sha, z_to,
         diff_bytes, diff_path, status) = parts
        try:
            with open(diff_path, "r", errors="replace") as df:
                diff_unified = df.read()
        except FileNotFoundError:
            diff_unified = ""
        results.append({
            "verb": verb,
            "invocation": inv,
            "args": args,
            "cwd": cwd,
            "status": status,
            "go": {
                "exit": g_exit,
                "stdout_bytes": int(g_so_b),
                "stderr_bytes": int(g_se_b),
                "stdout_sha256": g_so_sha,
                "stderr_sha256": g_se_sha,
                "timed_out": g_to == "true",
            },
            "zig": {
                "exit": z_exit,
                "stdout_bytes": int(z_so_b),
                "stderr_bytes": int(z_se_b),
                "stdout_sha256": z_so_sha,
                "stderr_sha256": z_se_sha,
                "timed_out": z_to == "true",
            },
            "diff": {
                "size_bytes": int(diff_bytes),
                "unified": diff_unified,
            },
        })

skipped = []
with open(skipped_path) as f:
    for line in f:
        line = line.rstrip("\n")
        if not line:
            continue
        verb, reason = line.split("|", 1)
        skipped.append({"verb": verb, "reason": reason})

doc = {
    "generated_at": generated_at,
    "go_bin": go_bin,
    "go_bin_sha256": go_sha,
    "zig_bin": zig_bin,
    "zig_bin_sha256": zig_sha,
    "audit_db": audit_db,
    "cwd_fixture_db": cwd_db,
    "cwd_fixture_dir": cwd_dir,
    "summary": {
        "verbs": int(verbs),
        "invocations": int(invocations),
        "gaps": int(gaps),
        "no_diff": int(no_diff),
        "errors": int(errors),
        "skipped": int(skipped_n),
    },
    "skipped": skipped,
    "results": results,
}

with open(out_path, "w") as f:
    json.dump(doc, f, indent=2, sort_keys=True)
    f.write("\n")
PY

log "wrote: $JSON_OUT"

# ---------- emit markdown ----------

title "writing markdown report"

MD_OUT="$OUT_DIR/parity-gap-report.md"

python3 - "$JSON_OUT" "$MD_OUT" <<'PY'
import json, sys

src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    doc = json.load(f)

s = doc["summary"]
lines = []
lines.append(f"# Parity Gap Report")
lines.append("")
lines.append(f"_Generated {doc['generated_at']}_")
lines.append("")
lines.append("## Summary")
lines.append("")
lines.append(
    f"{s['verbs']} verbs audited, "
    f"{s['gaps']} gaps surfaced, "
    f"{s['errors']} errors, "
    f"{s['skipped']} skipped (with reasons), "
    f"{s['no_diff']} no-diff invocations across "
    f"{s['invocations']} total invocations."
)
lines.append("")
lines.append(f"- Go binary: `{doc['go_bin']}` (sha256 `{doc['go_bin_sha256'][:12]}…`)")
lines.append(f"- Zig binary: `{doc['zig_bin']}` (sha256 `{doc['zig_bin_sha256'][:12]}…`)")
lines.append(f"- Audit DB: `{doc['audit_db']}`")
lines.append(f"- Cwd-fixture DB: `{doc['cwd_fixture_db']}`")
lines.append(f"- Cwd-fixture dir: `{doc['cwd_fixture_dir']}`")
lines.append("")

# Skipped verbs.
if doc["skipped"]:
    lines.append("## Pre-skipped verbs")
    lines.append("")
    lines.append("| Verb | Reason |")
    lines.append("|------|--------|")
    for sk in sorted(doc["skipped"], key=lambda x: x["verb"]):
        lines.append(f"| `{sk['verb']}` | {sk['reason']} |")
    lines.append("")

# Errors.
errs = [r for r in doc["results"] if r["status"] == "error"]
if errs:
    lines.append("## Errors")
    lines.append("")
    lines.append("Invocations where one or both binaries timed out or refused to launch. "
                 "Phase 2.5 needs to inspect these manually — they may be parser bugs, "
                 "infinite-loops, or genuine missing-verb errors.")
    lines.append("")
    lines.append("| Verb | Invocation | Args | Go exit | Go to | Zig exit | Zig to |")
    lines.append("|------|------------|------|---------|-------|----------|--------|")
    for r in sorted(errs, key=lambda x: (x["verb"], x["invocation"])):
        lines.append(
            f"| `{r['verb']}` | `{r['invocation']}` | `{r['args']}` | "
            f"{r['go']['exit']} | {r['go']['timed_out']} | "
            f"{r['zig']['exit']} | {r['zig']['timed_out']} |"
        )
    lines.append("")

# Gaps sorted by diff size desc.
gaps = [r for r in doc["results"] if r["status"] == "gap"]
gaps.sort(key=lambda r: r["diff"]["size_bytes"], reverse=True)

lines.append("## Gaps (by diff size, descending)")
lines.append("")
if not gaps:
    lines.append("_None._")
    lines.append("")
else:
    lines.append("| # | Verb | Invocation | Args | Diff bytes | Go exit | Zig exit |")
    lines.append("|---|------|------------|------|------------|---------|----------|")
    for i, r in enumerate(gaps, 1):
        lines.append(
            f"| {i} | `{r['verb']}` | `{r['invocation']}` | `{r['args']}` | "
            f"{r['diff']['size_bytes']} | {r['go']['exit']} | {r['zig']['exit']} |"
        )
    lines.append("")
    lines.append("### Per-gap diffs")
    lines.append("")
    for i, r in enumerate(gaps, 1):
        lines.append(f"#### {i}. `{r['verb']} {r['args']}` — invocation `{r['invocation']}`")
        lines.append("")
        lines.append(f"- Go exit: `{r['go']['exit']}` (stdout {r['go']['stdout_bytes']}B, stderr {r['go']['stderr_bytes']}B)")
        lines.append(f"- Zig exit: `{r['zig']['exit']}` (stdout {r['zig']['stdout_bytes']}B, stderr {r['zig']['stderr_bytes']}B)")
        if r["cwd"]:
            lines.append(f"- cwd: `{r['cwd']}`")
        lines.append("")
        diff = r["diff"]["unified"]
        # Truncate very large diffs in the markdown view; the JSON still has the full body.
        cap = 8000
        truncated = False
        if len(diff) > cap:
            diff = diff[:cap]
            truncated = True
        lines.append("```diff")
        lines.append(diff.rstrip("\n"))
        lines.append("```")
        if truncated:
            lines.append("")
            lines.append(f"_(diff truncated at {cap} bytes; full body in parity-gap-report.json)_")
        lines.append("")

# No-diff table at the bottom.
nd = [r for r in doc["results"] if r["status"] == "no_diff"]
nd.sort(key=lambda r: (r["verb"], r["invocation"]))
lines.append("## No-diff invocations")
lines.append("")
if not nd:
    lines.append("_None._")
else:
    lines.append("| Verb | Invocation | Args |")
    lines.append("|------|------------|------|")
    for r in nd:
        lines.append(f"| `{r['verb']}` | `{r['invocation']}` | `{r['args']}` |")
lines.append("")

with open(dst, "w") as f:
    f.write("\n".join(lines))
PY

log "wrote: $MD_OUT"

# ---------- final summary ----------

title "parity-audit done"
log "verbs:        $verb_count"
log "invocations:  $inv_count"
log "gaps:         $gap_count"
log "no_diff:      $no_diff_count"
log "errors:       $error_count"
log "skipped:      $skipped_count"
log ""
log "JSON: $JSON_OUT"
log "MD:   $MD_OUT"
log ""
log "Phase 2.5 (task 2364) consumes these artifacts — do not triage here."

# Best-effort cleanup of the tmp work dir. We keep $TMP_ROOT around so a
# reviewer can inspect raw outputs if curiosity arises; the next run
# uses a fresh tmp dir.
log "tmp work dir: $TMP_ROOT"

exit 0
