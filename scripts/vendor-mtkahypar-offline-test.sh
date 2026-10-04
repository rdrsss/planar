#!/usr/bin/env bash
set -euo pipefail

# Recurrence guard for task 6461: PLANAR_WITH_MTKAHYPAR=ON must configure
# entirely from the committed vendor/mtkahypar/, vendor/kahypar_shared_resources/,
# and vendor/whfc/ trees. CPM derives an un-pinned package's cache directory
# name from a SHA1 of that CPMAddPackage call's own arguments -- and for
# mt-kahypar specifically that included an ABSOLUTE path (the PATCHES file,
# folded in via CPM's PATCH_COMMAND machinery), so the resolved directory used
# to depend on the checkout's absolute path rather than only on the pinned
# archive + its options. A worktree or clone at a different path silently
# refetched from the network even though nothing about the dependency itself
# had changed (see cmake/dependencies.cmake's CUSTOM_CACHE_KEY comment on the
# mtkahypar block for the full story). This script configures with the
# solver ON under a network-denying sandbox and fails loudly if that ever
# regresses -- run it after touching cmake/dependencies.cmake's mtkahypar
# block or CPMAddPackage's cache-keying inputs.
#
# Requires the committed vendor/{mtkahypar,kahypar_shared_resources,whfc}/
# trees to already be populated (a normal checkout has them).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

if ! command -v sandbox-exec >/dev/null 2>&1; then
  echo "vendor-mtkahypar-offline-test: sandbox-exec not found (macOS only) -- skipping" >&2
  exit 0
fi

LOG="$BUILD_DIR/configure.log"

if ! sandbox-exec -p '(version 1)(allow default)(deny network*)' \
  cmake --preset debug -B "$BUILD_DIR" -S "$ROOT" -DPLANAR_WITH_MTKAHYPAR=ON \
  >"$LOG" 2>&1; then
  echo "FAIL: offline configure with -DPLANAR_WITH_MTKAHYPAR=ON did not succeed under a" >&2
  echo "denied network. This regresses the hermeticity task 6461 fixed -- see" >&2
  echo "cmake/dependencies.cmake's mtkahypar CUSTOM_CACHE_KEY comment." >&2
  echo "--- configure log ---" >&2
  cat "$LOG" >&2
  exit 1
fi

# A hermetic configure must resolve the three committed cache identities
# unchanged -- not fetch a fresh one under a new hash.
for pkg_dir in \
  "$ROOT/vendor/mtkahypar/1.6.2" \
  "$ROOT/vendor/kahypar_shared_resources/1e2f" \
  "$ROOT/vendor/whfc/e9cf"; do
  if ! grep -Fq "$pkg_dir" "$LOG"; then
    echo "FAIL: offline configure did not resolve the expected committed cache" >&2
    echo "directory: $pkg_dir" >&2
    echo "This means CPM derived a different (uncommitted) cache identity --" >&2
    echo "the exact regression task 6461 fixed. See configure log:" >&2
    cat "$LOG" >&2
    exit 1
  fi
done

printf 'vendor mtkahypar offline test: 1 passed\n'
