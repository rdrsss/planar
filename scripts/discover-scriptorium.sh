#!/usr/bin/env bash
# discover-scriptorium.sh — sourced by install.sh, not executed. Resolves the
# scriptorium binary that renders Planar's skill/agent vendor surfaces,
# replacing the retired in-tree `planar skills render` (plan 918 tech-spec
# § Architecture "How Planar shells scriptorium", decision D2).
#
# Discovery order (D2): an explicit $SCRIPTORIUM_BIN override wins; else a
# PATH lookup via `command -v scriptorium`. A missing or non-executable
# binary is a fatal, actionable error — never a silent skip (product-spec
# acceptance #2). Relies on install.sh's err()/warn()/log()/vlog()/
# version_ge() helpers already being defined by the time
# discover_scriptorium_bin is actually called (it is sourced early but
# invoked later, during preflight).

# SCRIPTORIUM_MIN_VERSION — the enforced floor. Q2 (plan 918 tech-spec:
# discovery order + bootstrap policy) is answered (2026-07-22): discovery is
# PATH lookup with a $SCRIPTORIUM_BIN override, and a missing/old binary
# aborts with an actionable message rather than auto-bootstrapping via
# `go install` (D2). What remains genuinely open is mechanical, not a
# decision: as of this writing scriptorium's CLI surface is render/init/
# install/uninstall/check/status/update/sync — there is no `--version`/
# `version` verb yet to compare against this floor. TODO(plan 918 tech-spec,
# scriptorium-side ask — file separately): once scriptorium ships a version
# verb, replace scriptorium_check_floor's fallback probe below with a real
# `version_ge` gate against this value.
SCRIPTORIUM_MIN_VERSION="0.1.0"

# scriptorium_check_floor <bin> — best-effort version-floor probe. Tries
# `--version` first (honored automatically the day scriptorium — or a test
# stub standing in for it — implements one); falls back to a binary-identity
# sanity probe (today's real scriptorium always takes this path, since
# `--version` is an unrecognized top-level command there and exits non-zero).
#
# The `--version` probe brackets install.sh's ERR trap with `trap - ERR` /
# restore rather than an `if`/`||` guard: install.sh runs under `set -eE`,
# and -E (errtrace) propagates the ERR trap into the command-substitution
# subshell below, where the probed binary's own non-zero exit fires the
# (inherited) trap immediately — independent of any `if`/`||` wrapping in
# THIS shell, and before that wrapping ever gets a chance to exempt it.
# Explicitly unsetting the trap for the probe's duration is the only
# reliable way to treat an expected non-zero exit here as data, not a fatal
# install failure (verified empirically; do not "simplify" this back to a
# bare `if out=$(...); then`).
# On failure, scriptorium_check_floor also sets SCRIPTORIUM_FLOOR_FAIL_REASON
# so the caller (discover_scriptorium_bin) can report a message that matches
# what actually failed, instead of a single message that misattributes a
# below-floor version mismatch to the sanity-probe fallback (or vice versa):
#   "below-floor:<version>" — --version worked but version_ge rejected it.
#   "sanity-probe"          — no --version support; the render --help
#                             fallback probe didn't look like scriptorium.
scriptorium_check_floor() {
  local bin="$1" out ver rc=0
  SCRIPTORIUM_FLOOR_FAIL_REASON=""
  trap - ERR
  out="$("$bin" --version 2>&1)" && rc=0 || rc=$?
  trap 'on_err $? $LINENO' ERR
  if [[ $rc -eq 0 ]]; then
    ver="${out##* }"
    if version_ge "$ver" "$SCRIPTORIUM_MIN_VERSION"; then
      return 0
    fi
    SCRIPTORIUM_FLOOR_FAIL_REASON="below-floor:$ver"
    return 1
  fi
  # No --version support today — confirm this is recognizably scriptorium's
  # `render` verb (its usage text names the flags render_cmd.go registers)
  # rather than silently accepting any executable named "scriptorium". The
  # `|| true` here is inside the command-substitution subshell itself, so
  # it exempts the failing command in the same context it runs in — safe,
  # unlike the `--version` probe above.
  out="$("$bin" render --help 2>&1 || true)"
  if [[ "$out" == *"-config string"* && "$out" == *"-source string"* ]]; then
    return 0
  fi
  SCRIPTORIUM_FLOOR_FAIL_REASON="sanity-probe"
  return 1
}

# discover_scriptorium_bin — sets the global SCRIPTORIUM_BIN to a resolved,
# executable, floor-checked scriptorium binary, or calls err() (fatal,
# non-zero exit, actionable message — never a silent skip).
discover_scriptorium_bin() {
  local candidate=""

  if [[ -n "${SCRIPTORIUM_BIN:-}" ]]; then
    candidate="$SCRIPTORIUM_BIN"
    [[ -x "$candidate" ]] || err "SCRIPTORIUM_BIN=$candidate is not an executable file. Point SCRIPTORIUM_BIN at a valid scriptorium binary (>= $SCRIPTORIUM_MIN_VERSION), or unset it to discover scriptorium on PATH."
  else
    candidate="$(command -v scriptorium 2>/dev/null || true)"
    [[ -n "$candidate" ]] || err "scriptorium binary not found on PATH (requires >= $SCRIPTORIUM_MIN_VERSION). Install scriptorium and ensure it is on PATH, or set SCRIPTORIUM_BIN to an explicit binary path."
  fi

  if ! scriptorium_check_floor "$candidate"; then
    case "$SCRIPTORIUM_FLOOR_FAIL_REASON" in
      below-floor:*)
        err "$candidate reports version ${SCRIPTORIUM_FLOOR_FAIL_REASON#below-floor:}, which is below the required floor $SCRIPTORIUM_MIN_VERSION. Upgrade scriptorium, or point SCRIPTORIUM_BIN at a binary >= $SCRIPTORIUM_MIN_VERSION."
        ;;
      *)
        err "$candidate does not look like a working scriptorium (sanity probe against 'scriptorium render --help' failed; scriptorium has no --version verb yet, so this probe stands in for a real floor check). Upgrade scriptorium, or point SCRIPTORIUM_BIN at a valid binary."
        ;;
    esac
  fi

  SCRIPTORIUM_BIN="$candidate"
  vlog "scriptorium: using $SCRIPTORIUM_BIN"
}
