#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

command -v scriptorium >/dev/null 2>&1 || {
  echo "error: scriptorium binary not found" >&2
  exit 1
}
command -v jq >/dev/null 2>&1 || {
  echo "error: jq binary not found" >&2
  exit 1
}

# `status` exits non-zero when the global manifest contains artifacts from
# another content repo that are undefined by this config. Preserve its JSON and
# apply the repo-scoped filter below.
STATUS="$(scriptorium status -config "$ROOT/scriptorium.yaml" -json || true)"
printf '%s' "$STATUS" |
  jq -e 'type == "object" and (.artifacts | type == "array")' >/dev/null || {
    echo "error: scriptorium status did not return a valid artifact report" >&2
    exit 1
  }

# Per-vendor install fan-out differs by artifact kind and is owned by
# install.sh; this gate checks the invariants that hold for every kind:
# rendered, undrifted, and installed to at least one vendor surface.
failures="$(
  printf '%s' "$STATUS" |
    jq -r '
      .artifacts[]
      | select(.defined == true)
      | select(
          .rendered != true
          or .drifted == true
          or ((.installed | length) == 0)
        )
      | "\(.kind):\(.slug) rendered=\(.rendered) drifted=\(.drifted) installed=\(.installed | join(","))"
    '
)"

if [ -n "$failures" ]; then
  echo "Planar installed projections are stale or missing:" >&2
  printf '%s\n' "$failures" >&2
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
echo "PASS: $count Planar source artifacts are rendered and installed without drift"
