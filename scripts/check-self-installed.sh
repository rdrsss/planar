#!/usr/bin/env bash
# Check that this host's installed Planar skill and agents match the staged
# authority under $PLANAR_HOME. The comparison is `planar health`'s
# installed-surface classifier (the nine vendor roots, against
# $PLANAR_HOME/skills/planar, agents and codex-agents); this script only reads
# its `projection_freshness` contributor.
set -euo pipefail

PLANAR_HOME="${PLANAR_HOME:-$HOME/.planar}"
PLANAR_BIN="$PLANAR_HOME/bin/planar"
[ -x "$PLANAR_BIN" ] || {
  echo "error: installed planar binary not found: $PLANAR_BIN" >&2
  exit 1
}
command -v jq >/dev/null 2>&1 || {
  echo "error: jq binary not found" >&2
  exit 1
}

HEALTH="$("$PLANAR_BIN" health --json)" || {
  echo "error: planar health failed" >&2
  exit 1
}
printf '%s' "$HEALTH" |
  jq -e '.projection_freshness | type == "object"' >/dev/null || {
    echo "error: planar health did not return a projection_freshness report" >&2
    exit 1
  }

manifest_status="$(printf '%s' "$HEALTH" | jq -r '.projection_freshness.manifest_status')"
managed="$(printf '%s' "$HEALTH" | jq -r '.projection_freshness.managed')"
stale="$(printf '%s' "$HEALTH" | jq -r '.projection_freshness.stale')"
missing="$(printf '%s' "$HEALTH" | jq -r '.projection_freshness.missing')"

if [ "$manifest_status" != "current" ] || [ "$managed" -eq 0 ] || [ "$stale" -ne 0 ] || [ "$missing" -ne 0 ]; then
  echo "Planar installed projections are not current:" >&2
  echo "  manifest=$manifest_status managed=$managed stale=$stale missing=$missing" >&2
  printf '%s' "$HEALTH" | jq -r '.projection_freshness.evidence // empty' >&2
  echo "run: ./install.sh" >&2
  exit 1
fi

echo "PASS: $managed installed projections match the staged skill and agents"
