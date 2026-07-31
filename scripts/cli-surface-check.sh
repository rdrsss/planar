#!/usr/bin/env bash
#
# cli-surface-check.sh — assert that every top-level verb the planar binary
# reports under `planar --help` has a corresponding handler entry under
# src/cmd/planar/handlers/. Surfaces "I added a verb but forgot to wire a
# handler" and "I added a handler but forgot to register it" regressions.
#
# Original intent was Go-vs-Zig parity diffing during the M1-M19 port. After
# the M20 cutover the Go binary is retired; this script becomes the
# maintenance gate inside the zig-only repo.
#
# Usage:
#   ./scripts/cli-surface-check.sh              # uses ./bin/planar if present, else PATH
#   PLANAR_BIN=/path/to/planar ./scripts/cli-surface-check.sh
#
# Exit codes:
#   0  all verbs have a handler entry
#   1  one or more verbs are missing a handler
#   64 invocation error (missing binary, missing handlers dir)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HANDLERS_DIR="$REPO_ROOT/src/cmd/planar/handlers"

BIN="${PLANAR_BIN:-}"
if [[ -z "$BIN" ]]; then
  if [[ -x "$REPO_ROOT/bin/planar" ]]; then
    BIN="$REPO_ROOT/bin/planar"
  elif command -v planar >/dev/null 2>&1; then
    BIN="$(command -v planar)"
  else
    echo "cli-surface-check: planar binary not found (set PLANAR_BIN, run 'make build', or put planar on PATH)" >&2
    exit 64
  fi
fi

# Point probes at a scratch database.
#
# A planar binary opens and MIGRATES its database before doing anything — even
# `--help`, which is all this script runs. Without an override those probes hit
# the operator's real ~/.planar/planar.db and silently migrate it to whatever
# schema the probed binary carries, breaking every other installed binary until
# someone rolls the migration back by hand.
CLI_SURFACE_DB_DIR="$(mktemp -d)"
trap 'rm -rf "$CLI_SURFACE_DB_DIR"' EXIT
export PLANAR_DB="$CLI_SURFACE_DB_DIR/cli-surface-probe.db"

if [[ ! -d "$HANDLERS_DIR" ]]; then
  echo "cli-surface-check: handlers dir missing: $HANDLERS_DIR" >&2
  exit 64
fi

# Verb-to-handler-name aliases for cases where the cobra-style verb name
# differs from the handler directory/file name on disk.
alias_for() {
  case "$1" in
    assoc) echo "association" ;;
    *)     echo "$1" ;;
  esac
}

# Look for either <name>.zig or <name>/ under HANDLERS_DIR. Returns 0 if
# found, 1 otherwise. The verb passed in is normalized: dashes → underscores.
handler_exists() {
  local raw="$1"
  local normalized="${raw//-/_}"
  local aliased
  aliased="$(alias_for "$normalized")"
  [[ -f "$HANDLERS_DIR/$normalized.zig"  ]] && return 0
  [[ -d "$HANDLERS_DIR/$normalized"      ]] && return 0
  [[ -f "$HANDLERS_DIR/$aliased.zig"     ]] && return 0
  [[ -d "$HANDLERS_DIR/$aliased"         ]] && return 0
  return 1
}

# Extract the verb list from `planar --help`. Cobra-style help renders each
# top-level command as a two-space-indented line: "  <verb>  <description>".
# Filter out lines that don't look like verbs (continuation lines, blank
# lines, section headers, the program name itself).
verbs_raw=$("$BIN" --help 2>&1 \
  | awk '/^  [a-z]/ { print $1 }' \
  | grep -v '^planar$' \
  | sort -u)

verb_count=0
missing=""
missing_count=0

# Iterate the verb list with newline IFS so each line is one verb.
while IFS= read -r v; do
  [[ -z "$v" ]] && continue
  verb_count=$((verb_count + 1))
  if ! handler_exists "$v"; then
    missing="${missing}${v}"$'\n'
    missing_count=$((missing_count + 1))
  fi
done <<< "$verbs_raw"

if [[ "$verb_count" -eq 0 ]]; then
  echo "cli-surface-check: no verbs parsed from $BIN --help — output format may have changed" >&2
  exit 64
fi

printf '%s verbs checked against %s\n' "$verb_count" "$HANDLERS_DIR"

if [[ "$missing_count" -eq 0 ]]; then
  printf 'OK — every verb has a handler entry.\n'
  exit 0
fi

printf 'FAIL — %d verb(s) missing a handler entry:\n' "$missing_count"
while IFS= read -r v; do
  [[ -z "$v" ]] && continue
  printf '  %s  (expected: %s.zig or %s/)\n' "$v" "${v//-/_}" "${v//-/_}"
done <<< "$missing"
exit 1
