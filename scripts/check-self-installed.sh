#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PLANAR_HOME="${PLANAR_HOME:-$HOME/.planar}"
SCRIPTORIUM_BIN="$PLANAR_HOME/bin/scriptorium"
[ -x "$SCRIPTORIUM_BIN" ] || {
  echo "error: installed scriptorium binary not found: $SCRIPTORIUM_BIN" >&2
  exit 1
}
command -v jq >/dev/null 2>&1 || {
  echo "error: jq binary not found" >&2
  exit 1
}

# `status` exits non-zero when the global manifest contains artifacts from
# another content repo that are undefined by this config. Preserve its JSON and
# apply the repo-scoped filter below.
STATUS="$("$SCRIPTORIUM_BIN" status --config "$ROOT/scriptorium.yaml" --output-root "$PLANAR_HOME" --json || true)"
printf '%s' "$STATUS" |
  jq -e 'type == "object" and (.artifacts | type == "array")' >/dev/null || {
    echo "error: scriptorium status did not return a valid artifact report" >&2
    exit 1
  }

# Scriptorium checks staged rendering. install.sh owns vendor-specific
# projection layouts and records the installed inventory in its manifest.
render_failures="$(
  printf '%s' "$STATUS" |
    jq -r '
      .artifacts[]
      | select(.orphaned == true or (.defined == true and .rendered != true))
      | "\(.kind):\(.slug) rendered=\(.rendered) orphaned=\(.orphaned)"
    '
)"

if [ -n "$render_failures" ]; then
  echo "Planar source artifacts are not rendered:" >&2
  printf '%s\n' "$render_failures" >&2
  echo "run: ./install.sh" >&2
  exit 1
fi

count="$(
  printf '%s' "$STATUS" |
    jq '[.artifacts[] | select(.defined == true)] | length'
)"
[ "$count" -gt 0 ] || {
  echo "error: scriptorium status found no Planar source artifacts" >&2
  exit 1
}

MANIFEST="${PLANAR_HOME:-$HOME/.planar}/install-manifest.json"
jq -e 'type == "object" and (.projections | type == "array") and (.projections | length > 0)' "$MANIFEST" >/dev/null 2>&1 || {
  echo "error: missing or invalid Planar install manifest: $MANIFEST" >&2
  exit 1
}

projection_failures="$(
  jq -r '.projections[] | [.vendor, .kind, .name, .staged_path, .installed_path] | @tsv' "$MANIFEST" |
    while IFS=$'\t' read -r vendor kind name staged installed; do
      if [ ! -f "$staged" ] || [ ! -f "$installed" ] || ! cmp -s "$staged" "$installed"; then
        printf '%s:%s:%s staged=%s installed=%s\n' "$vendor" "$kind" "$name" "$staged" "$installed"
      fi
    done
)"

if [ -n "$projection_failures" ]; then
  echo "Planar installed projections are stale or missing:" >&2
  printf '%s\n' "$projection_failures" >&2
  echo "run: ./install.sh" >&2
  exit 1
fi

projection_count="$(jq '.projections | length' "$MANIFEST")"
echo "PASS: $count Planar source artifacts are rendered and $projection_count installed projections are undrifted"
