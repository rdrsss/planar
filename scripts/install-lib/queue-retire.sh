# shellcheck shell=bash
#
# queue-retire.sh -- install.sh's queue-upgrade steps, as sourceable functions
# (plan 1089; tech spec 656, "Install and upgrade", decisions 1219-1229).
#
# install.sh sources this file. So do scripts/install-manifest-test.sh (with
# stub binaries, before the build) and the post-build ctest case
# `install_queue_probe_migrate` (with the real built binaries), which is why
# the steps live here rather than inline: a test drives exactly the code the
# installer runs.
#
# Inputs, read from the caller's environment:
#   PLANAR_HOME         the install prefix. Required.
#   IGNORE_LIVE_QUEUE   1 when --ignore-live-queue was passed. It is the ONLY
#                       override of the live-queue guard; --force is not one.
#
# Every function returns non-zero after printing why, and never exits: the
# caller decides what a refusal does (install.sh exits 1). Output goes
# through the caller's log/warn helpers when it defines them, so the
# installer's framing and warning count apply.
#
# The queue-store probe is classified in shell (_qr_classify_probe), so an
# install onto a prefix with a planar.db needs no python3 (decision 1333). The
# reader of the retired agent.db, scripts/install-lib/queue_retire.py, runs
# with python3 on the source path only. Paths reach it only through argv.
#
# install.sh runs under `set -eEuo pipefail` with an ERR trap (on_err), and
# -E carries that trap into command substitutions. Every substitution here
# whose command is EXPECTED to exit non-zero -- the probe (exit 1 not_found on
# almost every upgrade), init, and the store reader (exit 3 for a blocking
# row) -- therefore starts with `trap - ERR`. That clears the trap inside the
# substitution's own subshell only: the caller's trap is never touched, so
# there is nothing to restore, and the status still reaches `&& rc=0 ||
# rc=$?`. Without it on_err prints "install failed" while the install goes on.

QUEUE_RETIRE_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUEUE_RETIRE_PY="$QUEUE_RETIRE_LIB_DIR/queue_retire.py"

# The new queue's sequence floor (decision 1006): migration 00040 seeds
# sqlite_sequence at this value, so every new entry and its log is numbered
# above it. An old store that reached it cannot be told apart by number.
QUEUE_SEQ_FLOOR=1000000

# _qr_log / _qr_warn / _qr_fail -- print through install.sh's helpers when the
# caller defines them, else plainly. _qr_fail prints a refusal; it does not
# exit.
_qr_log() {
  if declare -F log >/dev/null; then log "$@"; else printf '  %s\n' "$*"; fi
}
_qr_warn() {
  if declare -F warn >/dev/null; then warn "$@"; else printf '  ! %s\n' "$*" >&2; fi
}
_qr_fail() {
  printf '\ninstall.sh: %s\n' "$*" >&2
}

# _qr_quote_cmd -- the migrate command as one line an operator can paste.
_qr_migrate_cmd_text() {
  printf '(cd / && PLANAR_DB=%q PLANAR_CONFIG_PATH=%q %q init --skip-project --allow-no-repo)' \
    "$PLANAR_HOME/planar.db" "$PLANAR_HOME/config.toml" "$PLANAR_HOME/bin/planar"
}

# _qr_classify_probe RC OUT -- classify one `planar-agent queue status 1
# --json` answer from its exit status RC and its stdout OUT, with grep and sed
# only. Sets QR_VERDICT (usable|behind|incompatible|foreign|failed) and
# QR_DETAIL. The mapping is the probe table of tech spec 656, "Steps, in
# order", step 3: exit 0 with a status object, or exit 1 with the error tag
# not_found, is usable; exit 125 with schema_version_behind,
# queue_schema_incompatible or queue_schema_foreign is behind, incompatible or
# foreign; everything else, a known tag on the wrong exit status included, is
# failed. The binary prints refusals as {"error":{"verb":..,"tag":"..",
# "message":..}} on stdout.
_qr_classify_probe() {
  local rc="$1" out="$2" flat tag="" shape is_object=0 has_error=0
  flat="$(printf '%s' "$out" | tr -d '\r\n')"
  if printf '%s' "$flat" | grep -qE '^[[:space:]]*\{.*\}[[:space:]]*$'; then is_object=1; fi
  if printf '%s' "$flat" | grep -qE '"error"[[:space:]]*:'; then
    has_error=1
    # The first "tag" string after the "error" object opens.
    tag="$(printf '%s' "$flat" \
      | sed -n 's/.*"error"[[:space:]]*:[[:space:]]*{\(.*\)$/\1/p' \
      | grep -oE '"tag"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 \
      | sed 's/.*:[[:space:]]*"\([^"]*\)"$/\1/' || true)"
  fi

  QR_VERDICT="failed"
  if [[ "$rc" == 0 && "$is_object" == 1 && "$has_error" == 0 ]]; then
    QR_VERDICT="usable"; QR_DETAIL="exit 0 with a status object"; return 0
  fi
  if [[ "$rc" == 1 && "$tag" == "not_found" ]]; then
    QR_VERDICT="usable"; QR_DETAIL="exit 1, tag not_found"; return 0
  fi
  if [[ "$rc" == 125 ]]; then
    case "$tag" in
      schema_version_behind)     QR_VERDICT="behind";       QR_DETAIL="exit 125, tag $tag"; return 0 ;;
      queue_schema_incompatible) QR_VERDICT="incompatible"; QR_DETAIL="exit 125, tag $tag"; return 0 ;;
      queue_schema_foreign)      QR_VERDICT="foreign";      QR_DETAIL="exit 125, tag $tag"; return 0 ;;
    esac
  fi
  if [[ "$is_object" == 0 ]]; then
    shape="output that is not JSON"
  elif [[ -z "$tag" ]]; then
    shape="JSON with no error tag"
  else
    shape="tag $tag"
  fi
  QR_DETAIL="exit $rc with $shape"
}

# _qr_probe -- ask the newly installed planar-agent, read-only and from /,
# whether the prefix planar.db is usable, and classify the answer in shell.
# Sets QR_VERDICT (usable|behind|incompatible|foreign|failed), QR_DETAIL and
# QR_PROBE_STDERR. Never fails itself: an unreadable answer is the verdict
# `failed`.
_qr_probe() {
  local db="$PLANAR_HOME/planar.db" out rc errf
  errf="$(mktemp)"
  out="$(trap - ERR; cd / && PLANAR_DB="$db" "$PLANAR_HOME/bin/planar-agent" queue status 1 --json 2>"$errf")" && rc=0 || rc=$?
  QR_PROBE_STDERR="$(cat "$errf" 2>/dev/null || true)"
  rm -f "$errf"
  _qr_classify_probe "$rc" "$out"
  _qr_log "queue store probe: $QR_VERDICT ($QR_DETAIL)"
}

# queue_probe_migrate -- tech spec 656 step 3 (decision 1008).
#
# Runs only when $PLANAR_HOME/planar.db exists, so it never creates one. The
# probe is the newly installed planar-agent's `queue status 1 --json`, run
# read-only from /. Behind: migrate with `planar init` from / and probe
# again, which must then be usable. Ahead with an incompatible or foreign
# queue schema: warn, naming which, and continue -- an ahead planar.db is
# never refused. Anything else: refuse, so the caller stops before agent.db
# is retired.
queue_probe_migrate() {
  local db="$PLANAR_HOME/planar.db"
  if [[ ! -e "$db" ]]; then
    _qr_log "no $db yet; skipping the queue store probe (it never creates one)"
    return 0
  fi
  _qr_probe
  case "$QR_VERDICT" in
    usable)
      return 0
      ;;
    behind)
      local cmd init_err init_out rc errf
      cmd="$(_qr_migrate_cmd_text)"
      _qr_log "planar.db is behind this build; migrating it:"
      _qr_log "  $cmd"
      errf="$(mktemp)"
      init_out="$(trap - ERR; cd / && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$PLANAR_HOME/config.toml" \
        "$PLANAR_HOME/bin/planar" init --skip-project --allow-no-repo 2>"$errf")" && rc=0 || rc=$?
      init_err="$(cat "$errf" 2>/dev/null || true)"
      rm -f "$errf"
      if [[ "$rc" -ne 0 ]]; then
        _qr_fail "planar.db migration failed: ${init_err:-exit $rc}; agent.db was NOT retired. The new binaries are installed. Run the command above by hand, then re-run ./install.sh:
  $cmd"
        return 1
      fi
      [[ -n "$init_out" ]] && _qr_log "$init_out"
      _qr_probe
      if [[ "$QR_VERDICT" != "usable" ]]; then
        _qr_fail "planar.db is still not usable after the migration ($QR_VERDICT: $QR_DETAIL${QR_PROBE_STDERR:+; $QR_PROBE_STDERR}); agent.db was NOT retired. Inspect it with: (cd / && PLANAR_DB=$(printf '%q' "$db") $(printf '%q' "$PLANAR_HOME/bin/planar-agent") queue status 1 --json), then re-run ./install.sh."
        return 1
      fi
      return 0
      ;;
    incompatible)
      _qr_warn "planar.db is ahead of this build and its queue tables changed; queue verbs from this install refuse at 125 until a newer build is installed"
      return 0
      ;;
    foreign)
      local n
      n="$(_qr_head_version_hint)"
      _qr_warn "planar.db's migration $n is not this build's migration $n (same number, foreign migration: two branches used the same number); queue verbs from this install refuse at 125 until the database or the build is corrected -- a newer build alone will not fix it"
      return 0
      ;;
    *)
      _qr_fail "the queue store probe of $db failed ($QR_DETAIL${QR_PROBE_STDERR:+; $QR_PROBE_STDERR}); agent.db was NOT retired. The new binaries are installed. Inspect it with: (cd / && PLANAR_DB=$(printf '%q' "$db") $(printf '%q' "$PLANAR_HOME/bin/planar-agent") queue status 1 --json), fix what it reports, then re-run ./install.sh."
      return 1
      ;;
  esac
}

# _qr_head_version_hint -- the migration number a foreign verdict is about,
# from the probe's own refusal text when it names one.
_qr_head_version_hint() {
  local n
  n="$(printf '%s' "$QR_PROBE_STDERR" | grep -oE 'version [0-9]+' | head -1 | grep -oE '[0-9]+' || true)"
  printf '%s' "${n:-<N>}"
}

# The documented cost of overriding the guard (tech spec 656, "Override").
_QR_OVERRIDE_COST="old submitters keep writing to the unlinked agent.db, their waiting commands still run when their turn comes in that orphaned queue, and those commands run outside the new queue's slot count until every old submitter drains"

# queue_live_guard WHEN -- the live-queue guard (tech spec 656 steps 1 and 4,
# and uninstall; decisions on questions 1006, 1007, 1009-1011).
#
# Runs only when $PLANAR_HOME/agent.db exists. Asks queue_retire.py `live`
# for a per-row judgement of the old store and refuses while any row blocks
# or the store cannot be read. WHEN names the step in the messages
# (preflight, re-check, uninstall). There is no process signal: a live old
# submitter holds its row for its whole run, so the store alone answers.
#
# IGNORE_LIVE_QUEUE=1 turns a blocking row, an unreadable store or a missing
# python3 into a warning. Nothing else does: --force is not an override.
queue_live_guard() {
  local when="$1" store="$PLANAR_HOME/agent.db" out rc
  [[ -e "$store" ]] || return 0
  local ignore="${IGNORE_LIVE_QUEUE:-0}"

  if ! command -v python3 >/dev/null 2>&1; then
    if [[ "$ignore" -eq 1 ]]; then
      _qr_warn "python3 is not installed, so $store was not checked for live queue entries ($when); continuing because --ignore-live-queue was passed: $_QR_OVERRIDE_COST"
      return 0
    fi
    _qr_fail "python3 is required to check $store for live queue entries before it is removed ($when). Install python3 and re-run, or pass --ignore-live-queue (--force does not bypass this check)."
    return 1
  fi

  out="$(trap - ERR; python3 "$QUEUE_RETIRE_PY" live "$store" 2>&1)" && rc=0 || rc=$?
  case "$rc" in
    0)
      local line
      while IFS= read -r line; do
        [[ "$line" == dead\ * ]] && _qr_log "agent.db: ignoring $line"
      done <<< "$out"
      return 0
      ;;
    3)
      if [[ "$ignore" -eq 1 ]]; then
        _qr_warn "the old queue in $store has live entries ($when); continuing because --ignore-live-queue was passed: $_QR_OVERRIDE_COST
$out"
        return 0
      fi
      _qr_fail "the old queue in $store has live entries ($when); refusing to retire it:
$out
Remedies: let those commands finish; or cancel them with the old \`planar-agent queue cancel <seq>\` if it is still installed; or re-run with --ignore-live-queue ($_QR_OVERRIDE_COST). --force does not bypass this check."
      return 1
      ;;
    *)
      if [[ "$ignore" -eq 1 ]]; then
        _qr_warn "$store could not be checked for live queue entries ($when): $out; continuing because --ignore-live-queue was passed (retire still refuses an unreadable store)"
        return 0
      fi
      _qr_fail "$store could not be checked for live queue entries ($when), so it is treated as live: $out"
      return 1
      ;;
  esac
}

# queue_retire_store -- tech spec 656 step 5: retire agent.db.
#
# Runs only when $PLANAR_HOME/agent.db exists, and only after the live-queue
# guard's re-check. Reads the old maximum sequence number and refuses when it
# cannot, or when it reaches the new floor -- no flag bypasses either. Then
# removes the old numbered logs (queue-logs/<n>.log, n an integer at or below
# the old maximum) and agent.db with its -wal and -shm, printing each
# removal. Logs numbered above the old maximum, such as a new queue's
# (above 1,000,000), and every other file are kept.
queue_retire_store() {
  local store="$PLANAR_HOME/agent.db" out rc max
  [[ -e "$store" ]] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    _qr_fail "python3 is required to read the old maximum sequence number of $store; it was NOT retired. Install python3 and re-run ./install.sh."
    return 1
  fi
  out="$(trap - ERR; python3 "$QUEUE_RETIRE_PY" oldmax "$store" 2>&1)" && rc=0 || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    _qr_fail "cannot read the old maximum sequence number of $store; it was NOT retired: $out"
    return 1
  fi
  max="$out"
  if [[ ! "$max" =~ ^[0-9]+$ || ${#max} -gt 18 ]]; then
    _qr_fail "the old maximum sequence number of $store is not a number ($max); it was NOT retired. Delete agent.db, agent.db-wal, agent.db-shm and the old numbered logs in queue-logs/ by hand once no old queue command is running."
    return 1
  fi
  if (( 10#$max >= QUEUE_SEQ_FLOOR )); then
    _qr_fail "the old maximum sequence number of $store is $max, which reaches the new queue's floor $QUEUE_SEQ_FLOOR, so its logs cannot be told apart from new ones by number; it was NOT retired. Move the logs you want to keep out of $PLANAR_HOME/queue-logs/, then delete agent.db, agent.db-wal, agent.db-shm and the old logs by hand."
    return 1
  fi

  local log name
  for log in "$PLANAR_HOME/queue-logs"/*.log; do
    [[ -f "$log" || -L "$log" ]] || continue
    [[ -d "$log" ]] && continue
    name="${log##*/}"
    name="${name%.log}"
    [[ "$name" =~ ^[0-9]+$ && ${#name} -le 18 ]] || continue
    if (( 10#$name <= 10#$max )); then
      rm -f "$log"
      _qr_log "removed old queue log $log"
    fi
  done
  local f
  for f in agent.db agent.db-wal agent.db-shm; do
    if [[ -e "$PLANAR_HOME/$f" || -L "$PLANAR_HOME/$f" ]]; then
      rm -f "$PLANAR_HOME/$f"
      _qr_log "removed $PLANAR_HOME/$f"
    fi
  done
  _qr_log "retired agent.db (old maximum sequence number $max; the queue now lives in planar.db)"
  return 0
}
