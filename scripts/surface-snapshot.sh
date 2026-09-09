#!/usr/bin/env bash
# Surface snapshot gate for the CLI colocation refactor (task 6401,
# decision 1068).
#
# The refactor moves 260 declared paths and 48 tree nodes between files
# across four binaries. Nothing about that is supposed to change what the
# binaries EXPOSE, so this captures the exposed surface as digests and fails
# when one moves.
#
#   scripts/surface-snapshot.sh capture   # write scripts/surface-baseline.txt
#   scripts/surface-snapshot.sh verify    # diff against it; non-zero on drift
#
# Covers, per binary: the `schema` catalog, root `--help`, and `--help` for
# every leaf the catalog declares. The per-leaf walk is the point — the
# catalog alone would not notice a leaf whose help text stopped rendering.
#
# This is a fast local gate, not the durable pin. The durable pins are the
# catalog strings in src/cmd/*/parity.t.cpp, which ctest checks. Both must
# pass; this one just fails in seconds instead of minutes.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/debug/bin"
BASELINE="$ROOT/scripts/surface-baseline.txt"
BINS=(planar planar-agent planar-watch planar-ext)

mode="${1:-verify}"

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
# Never let a from-source binary touch the operator's database or ~/.planar.
export HOME="$scratch"
export PLANAR_DB="$scratch/snapshot.db"
export PLANAR_WORKBENCH_ROOT="$scratch/workbench"

emit() {
  local b exe leaves path
  for b in "${BINS[@]}"; do
    exe="$BIN/$b"
    if [[ ! -x "$exe" ]]; then
      echo "surface-snapshot: missing $exe — build build/debug first" >&2
      return 2
    fi
    printf '%s schema %s\n' "$b" "$("$exe" schema 2>/dev/null | shasum -a 256 | cut -d' ' -f1)"
    printf '%s help . %s\n' "$b" "$("$exe" --help 2>&1 | shasum -a 256 | cut -d' ' -f1)"

    # Every leaf the catalog declares, by its own `command` string.
    leaves="$("$exe" schema 2>/dev/null \
      | jq -r --arg b "$b" '.commands[] | select(.command != $b) | .command' 2>/dev/null | sort)"
    while IFS= read -r path; do
      [[ -z "$path" ]] && continue
      # Strip the binary name; the rest is the argv for --help.
      printf '%s help %s %s\n' "$b" "$path" \
        "$("$exe" ${path#"$b" } --help 2>&1 | shasum -a 256 | cut -d' ' -f1)"
    done <<< "$leaves"
  done
}

case "$mode" in
  capture)
    emit > "$BASELINE" || exit $?
    printf 'surface-snapshot: captured %s lines to %s\n' "$(wc -l < "$BASELINE" | tr -d ' ')" "${BASELINE#"$ROOT"/}"
    ;;
  verify)
    if [[ ! -f "$BASELINE" ]]; then
      echo "surface-snapshot: no baseline at ${BASELINE#"$ROOT"/}; run 'capture' first" >&2
      exit 2
    fi
    emit > "$scratch/now.txt" || exit $?
    if diff -u "$BASELINE" "$scratch/now.txt" > "$scratch/drift.diff"; then
      printf 'surface-snapshot: clean (%s surface points unchanged)\n' "$(wc -l < "$BASELINE" | tr -d ' ')"
      exit 0
    fi
    echo "surface-snapshot: SURFACE DRIFT — the refactor changed what a binary exposes" >&2
    cat "$scratch/drift.diff" >&2
    exit 1
    ;;
  *)
    echo "usage: surface-snapshot.sh [capture|verify]" >&2; exit 2 ;;
esac
