#!/usr/bin/env bash
#
# ctest-registry-check.sh — prove the ctest registry matches the binaries
# (plan 996, task 6790).
#
# ## Why this exists
#
# `catch_discover_tests` writes one `<target>-<hash>_tests.cmake` per test
# target, and `ctest` runs exactly what those files register. Nothing checks
# that the registry still corresponds to the binaries, and it can drift in
# BOTH directions while `ctest` keeps reporting "100% tests passed":
#
#   SHRINK — a truncated discovery file silently drops cases. A run that
#   executed 3,685 of 4,984 registered tests reported a clean pass, because
#   every test it DID run passed.
#
#   INFLATE — the discovery file is appended rather than truncated for a
#   target that is not rebuilt in the current cycle, so every test name
#   registers twice. Measured 2026-09-16: FIFTEEN targets sat at exactly 2x
#   their real count (engine_planning registering 724 for 362 real cases;
#   engine_search 24 `add_test` lines for 12 unique names), inflating the
#   reported total from its true 3,553 to 4,871. `ctest` dutifully ran 1,333
#   duplicate executions and reported a clean pass.
#
# ## Why not a baseline file
#
# A floor compared against the last observed ctest TOTAL cannot work, because
# that total is not a count of distinct tests — it is whatever the registry
# happens to say, which is the thing under suspicion. A checked-in baseline
# would also need bumping on every legitimate test addition, and a number
# that is edited routinely stops being read.
#
# Instead this compares, per target, the binary's OWN `--list-tests` count
# against the number of `add_test` lines registered for it. That needs no
# baseline, never drifts, and fails on a mismatch in either direction.
#
# ## Usage
#
#   scripts/ctest-registry-check.sh [--build-dir DIR]
#
# Exits 0 when every target agrees, 1 on any mismatch.

set -uo pipefail

build_dir="build/debug"
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) build_dir="${2:?--build-dir needs a path}"; shift 2 ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) printf 'ctest-registry-check: unknown argument %s\n' "$1" >&2; exit 2 ;;
  esac
done

if [ ! -d "$build_dir" ]; then
  printf 'ctest-registry-check: no build directory at %s\n' "$build_dir" >&2
  exit 2
fi

mismatches=0
targets=0
total_binary=0
total_registered=0

while IFS= read -r bin; do
  label="$(basename "$bin")"
  dir="$(dirname "$bin")"

  # The binary's own count. Catch2 prints "N test cases" as its last
  # non-empty line; a binary that cannot be listed is itself a failure.
  listed="$("$bin" --list-tests 2>/dev/null | grep -oE '^[0-9]+ test cases?' | grep -oE '^[0-9]+')"
  if [ -z "$listed" ]; then
    printf 'MISMATCH %-46s could not read --list-tests\n' "$label"
    mismatches=$((mismatches + 1))
    continue
  fi

  # What ctest will actually run for it.
  registered=0
  for f in "$dir/$label"-*_tests.cmake; do
    [ -e "$f" ] || continue
    registered=$((registered + $(grep -c '^add_test' "$f")))
  done

  targets=$((targets + 1))
  total_binary=$((total_binary + listed))
  total_registered=$((total_registered + registered))

  if [ "$listed" -ne "$registered" ]; then
    if [ "$registered" -eq 0 ]; then
      note="NOT REGISTERED — ctest runs none of them"
    elif [ "$registered" -gt "$listed" ]; then
      note="registry INFLATED (stale discovery file appended, not truncated)"
    else
      note="registry SHRUNK (truncated discovery file)"
    fi
    printf 'MISMATCH %-46s binary=%-5s registered=%-5s %s\n' \
      "$label" "$listed" "$registered" "$note"
    mismatches=$((mismatches + 1))
  fi
done < <(find "$build_dir" -type f -perm -u+x -name '*_tests' | sort)

if [ "$targets" -eq 0 ]; then
  printf 'ctest-registry-check: found no test binaries under %s — build first\n' "$build_dir" >&2
  exit 2
fi

printf 'ctest-registry-check: %d targets, %d cases in binaries, %d registered\n' \
  "$targets" "$total_binary" "$total_registered"

if [ "$mismatches" -ne 0 ]; then
  printf 'ctest-registry-check: FAILED — %d target(s) disagree.\n' "$mismatches" >&2
  printf 'Rebuild refreshes a stale file:  cmake --build %s\n' "$build_dir" >&2
  printf 'To force a clean registry:       find %s -name "*_tests-*_tests.cmake" -delete && cmake --build %s\n' \
    "$build_dir" "$build_dir" >&2
  exit 1
fi

printf 'ctest-registry-check: OK\n'
