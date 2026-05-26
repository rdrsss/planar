#!/usr/bin/env bash
#
# parity-check.sh — gating wrapper around scripts/parity-audit.sh.
#
# Phase 5 of plan 351 (parity-gap-tests). Runs the parity audit, diffs
# the resulting gap set against scripts/parity-allowlist.txt, and
# exits non-zero if any unsanctioned gap surfaces. This is the standing
# parity invariant gate — new behavioral drift between the zig and Go
# binaries fails CI the moment it lands.
#
# Two modes:
#
# - **Full**: when the archive Go binary is available (either at
#   $PLANAR_GO_BIN, or buildable from $ARCHIVE). Runs the audit,
#   filters via the allowlist, exits 1 on any unsanctioned diff.
#
# - **Lightweight skip**: when no Go binary is reachable, prints a
#   clear notice and exits 0. The zig integration suite
#   (`make test-integration`, run separately) is the always-on parity
#   gate; the Go cross-binary diff is the deeper-but-optional check
#   on top. See plan 351 design note §"Phase 5 — lock in".
#
# Defaults match scripts/parity-audit.sh — same env vars, same paths.
#
# Usage
#   scripts/parity-check.sh
#   ARCHIVE=/path/to/planar-go-archive scripts/parity-check.sh
#   PLANAR_GO_BIN=/path/to/planar-go scripts/parity-check.sh
#   scripts/parity-check.sh --strict   # fail (exit 2) when Go binary is unreachable

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AUDIT_SCRIPT="$REPO_ROOT/scripts/parity-audit.sh"
ALLOWLIST="$REPO_ROOT/scripts/parity-allowlist.txt"
GAP_REPORT="$REPO_ROOT/scripts/parity-data/parity-gap-report.json"

STRICT=0
for arg in "$@"; do
  case "$arg" in
    --strict) STRICT=1 ;;
    --help|-h)
      sed -n '2,30p' "$0"
      exit 0
      ;;
    *)
      echo "parity-check: unknown arg '$arg' (try --help)" >&2
      exit 64
      ;;
  esac
done

# ---------- locate Go binary ----------
GO_BIN="${PLANAR_GO_BIN:-$HOME/.planar-archive/bin/planar-go}"
ARCHIVE="${ARCHIVE:-$HOME/projects/github/rdrsss/planar-go-archive}"

go_reachable=0
if [[ -x "$GO_BIN" ]]; then
  go_reachable=1
elif [[ -d "$ARCHIVE/src" ]]; then
  # scripts/parity-audit.sh will build the Go binary into $GO_BIN.
  go_reachable=1
fi

if [[ "$go_reachable" -eq 0 ]]; then
  cat >&2 <<EOM
parity-check: Go reference binary not reachable.
  PLANAR_GO_BIN = $GO_BIN  (not executable)
  ARCHIVE       = $ARCHIVE  (no src/ directory)

The full zig ↔ Go diff is skipped. The zig integration suite
(make test-integration) remains the always-on parity gate.

To enable the full check, clone github.com:rdrsss/planar-go-archive.git
into \$ARCHIVE or build planar-go into \$PLANAR_GO_BIN.
EOM
  if [[ "$STRICT" -eq 1 ]]; then
    exit 2
  fi
  exit 0
fi

# ---------- run audit ----------
echo "==> parity-check: running scripts/parity-audit.sh"
"$AUDIT_SCRIPT" >/dev/null

if [[ ! -s "$GAP_REPORT" ]]; then
  echo "parity-check: audit did not produce $GAP_REPORT" >&2
  exit 1
fi

# ---------- diff against allowlist ----------
if [[ ! -s "$ALLOWLIST" ]]; then
  echo "parity-check: $ALLOWLIST missing or empty" >&2
  exit 1
fi

echo "==> parity-check: comparing gaps against allowlist"
python3 - "$GAP_REPORT" "$ALLOWLIST" <<'PY'
import json, sys

gap_report_path, allowlist_path = sys.argv[1], sys.argv[2]

with open(gap_report_path, "r", encoding="utf-8") as f:
    report = json.load(f)

current_gaps = sorted(
    f"{r['verb']}/{r['invocation']}"
    for r in report["results"]
    if r["diff"]["size_bytes"] > 0
)

allowlisted = set()
with open(allowlist_path, "r", encoding="utf-8") as f:
    for raw in f:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        allowlisted.add(line)

current_set = set(current_gaps)
unsanctioned = sorted(current_set - allowlisted)
disappeared = sorted(allowlisted - current_set)

print(f"  audited gaps: {len(current_set)}")
print(f"  allowlisted:  {len(allowlisted)}")
print(f"  unsanctioned: {len(unsanctioned)}")
print(f"  disappeared:  {len(disappeared)}")

if unsanctioned:
    print()
    print("FAIL: new behavioral drift surfaced. Triage each row against the")
    print("four parity buckets and either fix zig (Bucket 1/2) or add the")
    print("key to scripts/parity-allowlist.txt with a recorded rationale")
    print("on plan 351 (Bucket 3/4).")
    print()
    for k in unsanctioned[:40]:
        print(f"  - {k}")
    if len(unsanctioned) > 40:
        print(f"  ... and {len(unsanctioned) - 40} more.")
    sys.exit(1)

if disappeared:
    print()
    print("notice: the following allowlist entries no longer appear in the")
    print("audit. Consider removing them from scripts/parity-allowlist.txt:")
    for k in disappeared[:20]:
        print(f"  - {k}")
    if len(disappeared) > 20:
        print(f"  ... and {len(disappeared) - 20} more.")

print()
print("OK: every audited gap is allowlisted.")
PY
