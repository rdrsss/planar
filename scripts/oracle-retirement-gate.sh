#!/usr/bin/env bash
# Fail-closed precondition for the reviewed Zig-oracle cutover.
set -euo pipefail

SCRIPT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$SCRIPT_ROOT"
if [[ ${1:-} == --source-root ]]; then ROOT="$2"; shift 2; fi
BUILD_DIR="${BUILD_DIR:-$SCRIPT_ROOT/build/debug}"
TEST_BIN="$BUILD_DIR/src/cmd/planar/planar_cmd_planar_tests"
COMMON="$(git -C "$SCRIPT_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
ORACLE_ROOT="${COMMON%/.git}"
[[ -n "$ORACLE_ROOT" ]] || ORACLE_ROOT="$SCRIPT_ROOT"
ORACLE="$ORACLE_ROOT/zig/zig-out/bin/planar"
REFUSED=0

refuse() { printf 'refusal: %s\n' "$*"; REFUSED=1; }

inventory() {
  local binary="$1" file="$2" line= block=0 rest value
  [ -f "$file" ] || { refuse "missing generated inventory source: $file"; return; }
  while IFS= read -r line; do
    [[ $block -eq 0 && $line == *'k_unported[] = {'* ]] && { block=1; continue; }
    [[ $block -eq 1 && $line == *'};'* ]] && break
    [[ $block -eq 1 ]] || continue
    line="${line%%//*}"
    rest="$line"
    while [[ $rest == *'"'* ]]; do
      rest="${rest#*\"}"; [[ $rest == *'"'* ]] || { refuse "malformed generated inventory: $file"; return; }
      value="${rest%%\"*}"; rest="${rest#*\"}"
      printf '%s:%s (%s)\n' "$binary" "$value" "$([[ $binary == planar && $value == explore ]] && echo deferred-by-decision980 || echo pending-port)"
    done
  done < "$file"
  [[ $block -eq 1 ]] || refuse "missing k_unported initializer: $file"
}

printf 'oracle-retirement: collecting evidence from %s\n' "$ROOT"
[[ -d "$ROOT/zig" ]] || refuse 'target checkout has no zig/ tree'
[[ -x "$ORACLE" ]] || refuse 'shared Zig oracle binary is unavailable'
[[ -x "$TEST_BIN" ]] || refuse "state test binary is unavailable: $TEST_BIN"
if [[ $REFUSED -eq 0 ]]; then
  "$TEST_BIN" 'C++ and Zig agree on DATABASE STATE across an ordered planning sequence' --reporter compact \
    || refuse 'real catalog-derived state differential failed'
fi

UNPORTED="$({
  inventory planar "$ROOT/src/cmd/planar/surface.cpp"
  inventory planar-agent "$ROOT/src/cmd/planar-agent/surface.cpp"
  inventory planar-watch "$ROOT/src/cmd/planar-watch/surface.cpp"
} | sort)"
UNPORTED_COUNT="$(printf '%s\n' "$UNPORTED" | sed '/^$/d' | wc -l | tr -d ' ')"
printf 'unported inventory (%s): %s\n' "$UNPORTED_COUNT" "$UNPORTED"
[[ $UNPORTED_COUNT -eq 1 && $UNPORTED == 'planar:explore (deferred-by-decision980)' ]] || \
  refuse "unported inventory is not only planar:explore: $UNPORTED"

SKIPS="$(
  # This mirrors the macro source grammar, not a hand-maintained location
  # list: any added, removed, or moved oracle conditional is evidence.
  while IFS= read -r -d '' file; do
    awk -v root="$ROOT/" '
      {
        code = $0
        sub(/\/\/.*/, "", code)
        sub(/^[ \t]+/, "", code)
        if (index(code, "PLANAR_REQUIRE_ORACLE(") == 1 ||
            (index(code, "SKIP(") == 1 &&
             (index(code, "Zig oracle") || index(code, "zig reference")))) {
          path = FILENAME
          sub(root, "", path)
          print path ":" FNR
        }
      }
    ' "$file"
  done < <(find "$ROOT/src" -type f -name '*.cpp' -print0 | LC_ALL=C sort -z)
)"
SKIP_COUNT="$(printf '%s\n' "$SKIPS" | sed '/^$/d' | wc -l | tr -d ' ')"
printf 'oracle conditional skip sources (%s): %s\n' "$SKIP_COUNT" "$SKIPS"
[[ $SKIP_COUNT -eq 0 ]] || refuse 'oracle-conditional parity skips remain'

if [[ $REFUSED -ne 0 ]]; then
  printf 'oracle-retirement: REFUSED\n'
  exit 1
fi
printf 'oracle-retirement: READY\n'
