#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Run an isolated installer copy whose pinned C++ compiler path is unavailable.
# The dependency preflight must refuse before it can invoke CMake.
mkdir -p "$TMP/repo/scripts"
cp "$ROOT/install.sh" "$TMP/repo/install.sh"
cp "$ROOT/scripts/install-manifest.sh" "$ROOT/scripts/discover-scriptorium.sh" "$TMP/repo/scripts/"

perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang\+\+#/definitely/missing/planar-clang++#g' \
  "$TMP/repo/install.sh"

if "$TMP/repo/install.sh" --dry-run --no-vendor >"$TMP/stdout" 2>"$TMP/stderr"; then
  echo "expected missing pinned C++ compiler to abort install.sh" >&2
  exit 1
fi

grep -Fq 'missing required build tool(s)' "$TMP/stderr"
grep -Fq '/definitely/missing/planar-clang++' "$TMP/stderr"
! grep -Fq 'Building the Planar binaries' "$TMP/stdout"

printf 'install dependency tests: 1 passed\n'
