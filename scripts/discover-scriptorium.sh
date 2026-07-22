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

# SCRIPTORIUM_MIN_VERSION — a provisional floor. As of this writing
# scriptorium's CLI surface is render/init/install/uninstall/check/status/
# update/sync — there is no `--version`/`version` verb to check against a
# real floor yet. TODO(plan 918 tech-spec Q2, OPEN — operator): once
# scriptorium ships a version verb, replace scriptorium_check_floor's
# fallback probe below with a real `version_ge` gate against this value.
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
scriptorium_check_floor() {
  local bin="$1" out ver rc=0
  trap - ERR
  out="$("$bin" --version 2>&1)" && rc=0 || rc=$?
  trap 'on_err $? $LINENO' ERR
  if [[ $rc -eq 0 ]]; then
    ver="${out##* }"
    version_ge "$ver" "$SCRIPTORIUM_MIN_VERSION"
    return $?
  fi
  # No --version support today — confirm this is recognizably scriptorium's
  # `render` verb (its usage text names the flags render_cmd.go registers)
  # rather than silently accepting any executable named "scriptorium". The
  # `|| true` here is inside the command-substitution subshell itself, so
  # it exempts the failing command in the same context it runs in — safe,
  # unlike the `--version` probe above.
  out="$("$bin" render --help 2>&1 || true)"
  [[ "$out" == *"-config string"* && "$out" == *"-source string"* ]]
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

  scriptorium_check_floor "$candidate" || \
    err "$candidate does not look like a working scriptorium >= $SCRIPTORIUM_MIN_VERSION (sanity probe against 'scriptorium render --help' failed). Upgrade scriptorium, or point SCRIPTORIUM_BIN at a valid binary."

  SCRIPTORIUM_BIN="$candidate"
  vlog "scriptorium: using $SCRIPTORIUM_BIN"
}
