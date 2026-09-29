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
#
# What this actually catches (task 6652, reviewer caveat on task 6633):
# SIBLING ORDER IS MACHINE-CHECKED, not a hand-carried risk. Three
# independent probes during M11 (a sibling swap, an anchor deletion, a
# flag-default downgrade) were each caught here, via the `schema` digest
# and the per-node `--help` hashes -- both render in declaration order, so
# a reordered sibling changes the digest. Do not re-propagate the earlier
# "sibling order is the one thing a passing gate might miss" claim; it was
# checked against this gate and found false. (The one caveat that DID apply
# -- this gate protects order only for as long as `apply_surface` was the
# thing writing node order into these trees -- is now historical: task 6616
# deleted `apply_surface` entirely, see src/lib/cliapp/surface.cppm.)
#
# What IS free to normalize without moving these digests (task 6655): a
# node's FLAG-vs-POSITIONAL interleave is not observable here, because
# `--help` and the `schema` catalog both render positionals and flags as
# separate sections/arrays regardless of the declaration order in tree.cpp.
# FLAG-vs-FLAG order and POSITIONAL-vs-POSITIONAL sibling order both ARE
# observable and will fail verify if changed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/debug/bin"
BASELINE="$ROOT/scripts/surface-baseline.txt"
BINS=(planar planar-agent planar-watch planar-ext)

mode="${1:-verify}"

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
# Never let a from-source binary touch the operator's database or ~/.planar.
# The agent database (decision 1181; override PLANAR_AGENT_DB) is pinned
# beside PLANAR_DB rather than left to the HOME fallback (task 6996).
export HOME="$scratch"
export PLANAR_DB="$scratch/snapshot.db"
export PLANAR_AGENT_DB="$scratch/agent.db"
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
