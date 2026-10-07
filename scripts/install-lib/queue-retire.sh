# shellcheck shell=bash
# shellcheck disable=SC2034  # QR_VERDICT and QR_DETAIL are read by the caller (db-probe.sh)
#
# queue-retire.sh -- install.sh's queue-upgrade steps, as sourceable functions
# (plan 1089; tech spec 656, "Install and upgrade", decisions 1219-1229).
#
# install.sh sources this file. So does scripts/install-manifest-test.sh, which
# drives exactly the functions the installer runs, with stub binaries.
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
# _qr_classify_probe classifies `planar-agent queue status 1 --json` in shell,
# so the database probe (db-probe.sh, plan 1122) needs no python3 (decision
# 1333). The reader of the retired agent.db, scripts/install-lib/queue_retire.py,
# runs with python3 on the source path only. Paths reach it only through argv.
#
# install.sh runs under `set -eEuo pipefail` with an ERR trap (on_err), and
# -E carries that trap into command substitutions. Every substitution here
# whose command is EXPECTED to exit non-zero (the store reader exits 3 for a
# blocking row) therefore starts with `trap - ERR`. That clears the trap inside
# the substitution's own subshell only: the caller's trap is never touched, so
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

# _QR_JSON_AWK -- a small JSON scanner for the probe's answer, read on stdin
# with the line breaks already turned into spaces. It prints
# `<kind>|<has_error>|<tag>`:
#   kind       bad (not exactly one complete JSON value), object, array or
#              scalar;
#   has_error  1 when the TOP-LEVEL object has an "error" key (a nested one
#              does not count);
#   tag        the string value of "tag" inside the FIRST top-level "error"
#              object, at that object's own depth: a "tag" anywhere else,
#              later in the document or nested deeper, is ignored.
# It checks the grammar (balanced containers, a key before every colon, valid
# literals and numbers, nothing after the first complete value), so
# concatenated objects, a stray token and a leading byte-order mark all read
# as bad. String escapes are skipped, not decoded: a tag is compared as
# written, and the binary's tags are plain ASCII.
# shellcheck disable=SC2016  # the awk program is single-quoted on purpose
_QR_JSON_AWK='
function begin_value(isstr, str) {
  first_err = 0
  if (sp < 1 || typ[sp] != "o") return
  if (sp == 1 && key[1] == "error") {
    has_error = 1
    if (!err_seen) { err_seen = 1; first_err = 1 }
  }
  if (in_err && sp == 2 && key[2] == "tag" && !tag_done) {
    tag_done = 1
    if (isstr) tag = str
  }
}
{ s = s $0 }
END {
  n = length(s); i = 1; sp = 0; expect = "V"; done = 0; ok = 1
  has_error = 0; tag = ""; tag_done = 0; in_err = 0; err_seen = 0; kind = "scalar"
  while (i <= n && ok) {
    c = substr(s, i, 1)
    if (c == " " || c == "\t") { i++; continue }
    if (done) { ok = 0; break }
    if (c == "\"") {
      j = i + 1; str = ""
      while (j <= n) {
        d = substr(s, j, 1)
        if (d == "\\") { str = str d substr(s, j + 1, 1); j += 2; continue }
        if (d == "\"") break
        str = str d; j++
      }
      if (j > n) { ok = 0; break }
      i = j + 1
      if (expect == "K" || expect == "KE") { key[sp] = str; expect = "C" }
      else if (expect == "V" || expect == "VE") {
        begin_value(1, str)
        if (sp == 0) done = 1; else expect = "CE"
      } else ok = 0
      continue
    }
    if (c == "{" || c == "[") {
      if (expect != "V" && expect != "VE") { ok = 0; break }
      if (sp == 0) kind = (c == "{" ? "object" : "array")
      begin_value(0, "")
      sp++; typ[sp] = (c == "{" ? "o" : "a")
      if (first_err && c == "{") in_err = 1
      expect = (c == "{" ? "KE" : "VE"); i++; continue
    }
    if (c == "}" || c == "]") {
      want = (c == "}" ? "o" : "a")
      if (sp < 1 || typ[sp] != want) { ok = 0; break }
      if (!(expect == "CE" || (expect == "KE" && want == "o") || (expect == "VE" && want == "a"))) { ok = 0; break }
      if (in_err && sp == 2) in_err = 0
      sp--
      if (sp == 0) done = 1; else expect = "CE"
      i++; continue
    }
    if (c == ",") {
      if (expect != "CE") { ok = 0; break }
      expect = (typ[sp] == "o" ? "K" : "V"); i++; continue
    }
    if (c == ":") {
      if (expect != "C") { ok = 0; break }
      expect = "V"; i++; continue
    }
    # A bare literal or number: a run up to the next structural character.
    j = i
    while (j <= n && index(" \t{}[]:,\"", substr(s, j, 1)) == 0) j++
    tok = substr(s, i, j - i); i = j
    if (!(expect == "V" || expect == "VE")) { ok = 0; break }
    if (tok != "true" && tok != "false" && tok != "null" \
        && tok !~ /^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$/) { ok = 0; break }
    begin_value(0, "")
    if (sp == 0) done = 1; else expect = "CE"
  }
  if (!ok || !done || sp != 0) { kind = "bad"; has_error = 0; tag = "" }
  printf "%s|%d|%s\n", kind, has_error, tag
}
'

# _qr_classify_probe RC OUT -- classify one `planar-agent queue status 1
# --json` answer from its exit status RC and its stdout OUT, with tr and awk
# only. Sets QR_VERDICT (usable|behind|incompatible|foreign|failed) and
# QR_DETAIL. The mapping is the probe table of tech spec 656, "Steps, in
# order", step 3: exit 0 with a status object, or exit 1 with the error tag
# not_found, is usable; exit 125 with schema_version_behind,
# queue_schema_incompatible or queue_schema_foreign is behind, incompatible or
# foreign; everything else, a known tag on the wrong exit status included, is
# failed. The binary prints refusals as {"error":{"verb":..,"tag":"..",
# "message":..}} on stdout.
#
# The answer is read by a small grammar-checking scanner (_QR_JSON_AWK), not
# by patterns: the tag is the one in the first top-level "error" object, an
# "error" key nested in a successful answer does not make it a refusal, and
# output that is not exactly one JSON value (concatenated objects, trailing
# text, a byte-order mark) is failed.
_qr_classify_probe() {
  local rc="$1" out="$2" flat parsed kind has_error tag shape
  flat="$(printf '%s' "$out" | tr '\r\n' '  ')"
  parsed="$(trap - ERR; printf '%s\n' "$flat" | LC_ALL=C awk "$_QR_JSON_AWK")" || parsed="bad|0|"
  IFS='|' read -r kind has_error tag <<< "$parsed"

  QR_VERDICT="failed"
  if [[ "$rc" == 0 && "$kind" == object && "$has_error" == 0 ]]; then
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
  if [[ "$kind" == bad ]]; then
    shape="output that is not JSON"
  elif [[ -z "$tag" ]]; then
    shape="JSON with no error tag"
  else
    shape="tag $tag"
  fi
  QR_DETAIL="exit $rc with $shape"
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

# _qr_move_aside SRC DIR NAME -- move SRC to DIR/NAME without replacing
# anything: when DIR/NAME is taken, the first free DIR/NAME.<n> (n = 1, 2, ...)
# is used instead. Prints the move. Returns non-zero, after printing why, when
# the move fails.
_qr_move_aside() {
  local src="$1" dir="$2" name="$3" target n=1
  target="$dir/$name"
  while [[ -e "$target" || -L "$target" ]]; do
    target="$dir/$name.$n"
    n=$((n + 1))
  done
  if ! mv "$src" "$target"; then
    _qr_fail "could not move $src to $target; the old queue store was NOT retired. Move agent.db, agent.db-wal, agent.db-shm and the old numbered logs in queue-logs/ aside by hand, then re-run."
    return 1
  fi
  _qr_log "moved $src -> $target"
}

# queue_retire_prebuilt -- the prebuilt path's step 5 (tech spec 677, "Prebuilt
# install mode"): move the retired agent.db aside instead of reading and
# removing it.
#
# Runs only when $PLANAR_HOME/agent.db or one of its sidecars exists. It reads
# nothing and runs no python3: agent.db, agent.db-wal, agent.db-shm and the old
# numbered logs (queue-logs/<n>.log with n below the new queue's floor, which
# every new entry exceeds; the old store's maximum is not read) move into
# $PLANAR_HOME/retired/<YYYY-MM-DD>/ (logs under queue-logs/ there), each move
# printed. An existing retired/<date>/ is reused and nothing in it is replaced:
# when any of the three names is taken, all three move as agent.db.<n>,
# agent.db.<n>-wal and agent.db.<n>-shm (the first free <n>), keeping SQLite's
# database/sidecar pairing; a taken log name gets a numeric suffix. Logs numbered at or above the floor, such
# as a new queue's, and every other file stay. retired/ is a data path, so no
# install or uninstall removes what lands there.
queue_retire_prebuilt() {
  local home="$PLANAR_HOME" f have=0 day dest log name
  for f in agent.db agent.db-wal agent.db-shm; do
    if [[ -e "$home/$f" || -L "$home/$f" ]]; then have=1; fi
  done
  [[ "$have" -eq 1 ]] || return 0

  day="$(date +%Y-%m-%d)" || day=""
  if [[ ! "$day" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
    _qr_fail "could not read today's date, so the old queue store was NOT retired. Move agent.db, agent.db-wal, agent.db-shm and the old numbered logs in queue-logs/ aside by hand, then re-run."
    return 1
  fi
  dest="$home/retired/$day"
  if ! mkdir -p "$dest"; then
    _qr_fail "could not create $dest; the old queue store was NOT retired. Move agent.db, agent.db-wal, agent.db-shm and the old numbered logs in queue-logs/ aside by hand, then re-run."
    return 1
  fi
  chmod 700 "$home/retired" "$dest" 2>/dev/null || true

  # SQLite pairs a database with <db>-wal and <db>-shm by name, so the three
  # move under one shared name: agent.db, or the first agent.db.<n> for which
  # none of agent.db.<n>, agent.db.<n>-wal and agent.db.<n>-shm is taken.
  local base="agent.db" n=0
  while [[ -e "$dest/$base" || -L "$dest/$base" || -e "$dest/$base-wal" || -L "$dest/$base-wal" \
           || -e "$dest/$base-shm" || -L "$dest/$base-shm" ]]; do
    n=$((n + 1))
    base="agent.db.$n"
  done
  for f in agent.db agent.db-wal agent.db-shm; do
    if [[ -e "$home/$f" || -L "$home/$f" ]]; then
      _qr_move_aside "$home/$f" "$dest" "$base${f#agent.db}" || return 1
    fi
  done
  for log in "$home/queue-logs"/*.log; do
    [[ -f "$log" || -L "$log" ]] || continue
    name="${log##*/}"
    name="${name%.log}"
    [[ "$name" =~ ^[0-9]+$ && ${#name} -le 18 ]] || continue
    (( 10#$name < QUEUE_SEQ_FLOOR )) || continue
    mkdir -p "$dest/queue-logs" && chmod 700 "$dest/queue-logs" 2>/dev/null || true
    _qr_move_aside "$log" "$dest/queue-logs" "${log##*/}" || return 1
  done
  _qr_log "retired the old queue store into $dest without reading it (the queue now lives in planar.db)"
  return 0
}
