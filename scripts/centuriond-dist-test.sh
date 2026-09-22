#!/usr/bin/env bash
# centuriond-dist-test.sh — the installed centuriond runs with its source
# cache ABSENT (plan 1033 M1, task 6709; tech-spec D8).
#
# The claim under test: scripts/install-centuriond.sh produces a daemon that
# needs nothing from the Centurion source tree it was built from. A test that
# merely runs the freshly built daemon next to its sources cannot tell a
# self-contained install from one that silently reads migrations out of the
# cache. So this test:
#
#   1. clones the pinned source tree to a throwaway directory (an APFS clone
#      where the filesystem supports one, a plain copy otherwise);
#   2. source-builds and installs centuriond from THAT copy into a temp
#      prefix, through the same script install.sh uses (--no-release, so the
#      source path is the one exercised);
#   3. DELETES the copy and the build tree;
#   4. starts the installed daemon against a fresh, empty state directory
#      with no --migrations flag, so it must resolve its migrations from the
#      installed prefix, and asserts it migrates and exits 0;
#   5. asserts the binary's compiled-in migrations default is the installed
#      prefix, not the deleted copy, and that build-identity.json records the
#      pin.
#
# Slow — it compiles Centurion's gRPC stack from scratch (~10 min) — so it is
# an explicit target, `make centuriond-dist-test`, not part of `make test`.
#
# Usage: centuriond-dist-test.sh <planar-build-dir>   (default build/debug)

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
planar_build="${1:-$repo/build/debug}"
pin="$planar_build/centurion-pin.env"

fail() { printf 'centuriond-dist-test: FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'centuriond-dist-test: ok: %s\n' "$*"; }

[[ -f "$pin" ]] || fail "$pin missing — configure Planar first (cmake --preset debug)"
# shellcheck source=/dev/null
source "$pin"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
copy="$work/centurion-src"
prefix="$work/prefix"
state="$work/state"

cp -c -R "$CENTURION_SOURCE_DIR" "$copy" 2>/dev/null || cp -R "$CENTURION_SOURCE_DIR" "$copy"
# A stale build tree inside the cached source would be copied along with it.
rm -rf "$copy/build"

"$repo/scripts/install-centuriond.sh" --no-release \
  --pin "$pin" --source "$copy" \
  --build-dir "$work/build" --prefix "$prefix" \
  --toolchain "$repo/cmake/llvm-toolchain.cmake"

rm -rf "$copy" "$work/build"
[[ ! -e "$copy" ]] || fail "could not remove the source copy"
pass "source copy and build tree deleted"

bin="$prefix/bin/centuriond"
[[ -x "$bin" ]] || fail "no installed $bin"
# Only the MIGRATIONS default is checked: compiled-in source file names
# (std::source_location, assert messages) legitimately name the build tree and
# are never opened at run time.
#
# `strings` goes to a file first: under pipefail, `strings | grep -q` lets grep
# exit on its first match, strings dies of SIGPIPE, and the pipeline reports
# FAILURE for a string that IS present — inverting both checks below.
strings "$bin" >"$work/strings.txt"
if grep -qF "$copy/migrations" "$work/strings.txt"; then
  fail "installed centuriond defaults its migrations to the deleted source tree"
fi
grep -qF "$prefix/share/centurion/migrations" "$work/strings.txt" ||
  fail "installed centuriond does not default its migrations to the installed prefix"
pass "migrations default is the installed prefix, not the source tree"

mkdir -p "$state"
if ! "$bin" --database "$state/centurion.db" --migrate-only >"$work/daemon.log" 2>&1; then
  cat "$work/daemon.log" >&2
  fail "installed centuriond could not migrate a fresh state dir"
fi
[[ -s "$state/centurion.db" ]] || fail "centuriond exited 0 but wrote no database"
pass "installed centuriond migrated a fresh state dir from the installed migrations"

identity="$prefix/share/centurion/build-identity.json"
[[ -f "$identity" ]] || fail "no $identity"
for want in "\"tag\": \"$CENTURION_TAG\"" "\"commit\": \"$CENTURION_COMMIT\"" \
            "\"archive_sha256\": \"$CENTURION_SHA256\"" '"source": "source-build"' \
            "\"binary_sha256\": \"$(shasum -a 256 "$bin" | awk '{print $1}')\""; do
  grep -qF "$want" "$identity" || fail "build identity lacks $want"
done
pass "build identity records the pin, the source kind and the binary digest"
