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
#   PLANAR_HOME   the install prefix. Required.
#
# Every function returns non-zero after printing why, and never exits: the
# caller decides what a refusal does (install.sh exits 1). Output goes
# through the caller's log/warn helpers when it defines them, so the
# installer's framing and warning count apply.
#
# The store reader is scripts/install-lib/queue_retire.py, run with python3.
# Paths reach it only through argv.

QUEUE_RETIRE_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUEUE_RETIRE_PY="$QUEUE_RETIRE_LIB_DIR/queue_retire.py"

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

# _qr_probe -- ask the newly installed planar-agent, read-only and from /,
# whether the prefix planar.db is usable, and classify the answer with
# python3. Sets QR_VERDICT (usable|behind|incompatible|foreign|failed),
# QR_DETAIL and QR_PROBE_STDERR. Never fails itself: an unreadable answer is
# the verdict `failed`.
_qr_probe() {
  local db="$PLANAR_HOME/planar.db" out rc errf line
  errf="$(mktemp)"
  out="$(cd / && PLANAR_DB="$db" "$PLANAR_HOME/bin/planar-agent" queue status 1 --json 2>"$errf")" && rc=0 || rc=$?
  QR_PROBE_STDERR="$(cat "$errf" 2>/dev/null || true)"
  rm -f "$errf"
  line="$(printf '%s' "$out" | python3 "$QUEUE_RETIRE_PY" probe-verdict "$rc" 2>&1)" \
    || line="failed	the probe classifier failed: $line"
  QR_VERDICT="${line%%	*}"
  QR_DETAIL="${line#*	}"
  case "$QR_VERDICT" in
    usable|behind|incompatible|foreign) ;;
    *) QR_VERDICT="failed" ;;
  esac
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
  if ! command -v python3 >/dev/null 2>&1; then
    _qr_fail "python3 is required to read the queue store probe of $db; agent.db was NOT retired. Install python3, then re-run ./install.sh."
    return 1
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
      init_out="$(cd / && PLANAR_DB="$db" PLANAR_CONFIG_PATH="$PLANAR_HOME/config.toml" \
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
