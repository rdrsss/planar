#!/usr/bin/env bash
#
# break-probe.sh — run ONE break-probe against the C++ tree and assert it
# actually discriminates.
#
# A break-probe is the project's primary evidence that a test is not
# vacuous: mutate the implementation, confirm the NAMED test fails, restore,
# confirm it passes again. Every step of that sequence has a silent-failure
# mode, and every one of them looks exactly like success. This script exists
# so each cycle stops re-deriving them (plan 996, task 6124).
#
# ## The four traps, and what this script does about each
#
#   1. THE MTIME TRAP — the one that cost a whole probe pass (task 6124).
#      The natural restore, `mv F.bak F`, puts back the BACKUP's mtime,
#      which is OLDER than the mutated file's. Ninja compares mtimes, sees
#      nothing newer than the object file, and SKIPS the rebuild — so the
#      next build still has the MUTANT linked and every subsequent probe in
#      the pass measures the wrong binary. The pass then reports "all
#      mutants killed" while never having rebuilt after the first one.
#      This script restores by content and `touch`es the file, then rebuilds
#      and re-runs the test to prove the restore actually took.
#
#   2. THE ZERO-MATCH TRAP — a mutation whose anchor matches nothing leaves
#      the file byte-identical, the test passes, and "the test did not fail"
#      is indistinguishable from a survivor. This script diffs the file
#      before and after the mutation command and REFUSES to continue if
#      nothing changed.
#
#   3. THE COMPILER-REJECTED MUTANT — a mutant that does not build is not a
#      kill; it proves the compiler works, not the test. This script fails
#      loudly when the mutated tree does not build.
#
#   4. THE EMPTY TEST FILTER — `ctest -R 'some tag'` that matches zero tests
#      exits non-zero on some paths and zero on others, and either way
#      proves nothing. This script asserts the filter matches at least one
#      test NAME before it trusts any result from it (Catch2 TAGS are not
#      ctest names; catch_discover_tests registers the TEST_CASE title).
#
# ## Usage
#
#   scripts/break-probe.sh \
#     --file <path>            (repeatable: every file the mutation touches)
#     --mutate <shell command> (must actually change at least one --file)
#     --test <ctest -R regex>  (matches ctest test NAMES)
#     [--build-dir build/debug]
#     [--target <ninja target>]   restrict the rebuild; default: everything
#     [--label <text>]            what shows up in the probe table
#
# The mutation is supplied as a command rather than performed here so the
# caller can use whatever edit tool is appropriate (sed, an editor, a
# patch); the guarantees above hold regardless of how the edit is made.
#
# Exit status is 0 only when the full sequence held:
#   baseline green -> mutant builds -> named test FAILS -> restored ->
#   rebuilt -> named test GREEN again.
# Anything else is a non-zero exit with a diagnostic naming which step
# broke. A survivor (mutant builds, test still green) is reported as
# SURVIVOR and is a finding about the TEST, not about this script.

set -u -o pipefail

BUILD_DIR="build/debug"
TARGET=""
LABEL=""
MUTATE=""
TEST_RE=""
FILES=()

die() {
  printf 'break-probe: %s\n' "$*" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --file)      FILES+=("$2"); shift 2 ;;
    --mutate)    MUTATE="$2"; shift 2 ;;
    --test)      TEST_RE="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --target)    TARGET="$2"; shift 2 ;;
    --label)     LABEL="$2"; shift 2 ;;
    *)           die "unknown argument '$1'" ;;
  esac
done

[ "${#FILES[@]}" -gt 0 ] || die "at least one --file is required"
[ -n "$MUTATE" ]         || die "--mutate is required"
[ -n "$TEST_RE" ]        || die "--test is required"
[ -d "$BUILD_DIR" ]      || die "build directory '$BUILD_DIR' does not exist — configure it first"
[ -n "$LABEL" ]          || LABEL="$MUTATE"

for f in "${FILES[@]}"; do
  [ -f "$f" ] || die "--file '$f' does not exist"
done

BACKUP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/break-probe.XXXXXX")"
RESTORED=0

# Restore by CONTENT into the original path and then bump the mtime, rather
# than `mv`-ing the backup over it: see trap 1 above.
restore() {
  [ "$RESTORED" -eq 1 ] && return 0
  local i=0 f
  for f in "${FILES[@]}"; do
    cp "$BACKUP_DIR/$i" "$f" || die "could not restore '$f' from backup — the tree is MUTATED, fix it by hand"
    touch "$f"
    i=$((i + 1))
  done
  RESTORED=1
}

cleanup() {
  restore
  rm -rf "$BACKUP_DIR"
}
trap cleanup EXIT INT TERM

build() {
  if [ -n "$TARGET" ]; then
    cmake --build "$BUILD_DIR" --target "$TARGET" > "$BACKUP_DIR/build.log" 2>&1
  else
    cmake --build "$BUILD_DIR" > "$BACKUP_DIR/build.log" 2>&1
  fi
}

# Assert the filter names at least one real ctest test, then run it.
# Returns 0 when every matched test passed, 1 when at least one failed.
run_test() {
  local listing count
  listing="$(ctest --test-dir "$BUILD_DIR" -N -R "$TEST_RE" 2>&1)"
  count="$(printf '%s\n' "$listing" | sed -n 's/^Total Tests: //p')"
  if [ -z "$count" ] || [ "$count" -eq 0 ]; then
    printf 'break-probe: --test regex %s matches ZERO ctest test names.\n' "$TEST_RE" >&2
    printf 'Catch2 TAGS are not ctest names; catch_discover_tests registers the TEST_CASE title.\n' >&2
    return 2
  fi
  ctest --test-dir "$BUILD_DIR" -R "$TEST_RE" --output-on-failure > "$BACKUP_DIR/ctest.log" 2>&1
}

printf '== break-probe: %s\n' "$LABEL"
printf '   files : %s\n' "${FILES[*]}"
printf '   test  : %s\n' "$TEST_RE"

i=0
for f in "${FILES[@]}"; do
  cp "$f" "$BACKUP_DIR/$i" || die "could not back up '$f'"
  i=$((i + 1))
done

printf '   [1/6] baseline build ... '
if ! build; then
  printf 'FAILED\n'
  tail -30 "$BACKUP_DIR/build.log" >&2
  die "the UNMUTATED tree does not build — nothing this script reports afterwards would mean anything"
fi
printf 'ok\n'

printf '   [2/6] baseline test ... '
run_test
case $? in
  0) printf 'green\n' ;;
  2) exit 2 ;;
  *) printf 'FAILED\n'; tail -40 "$BACKUP_DIR/ctest.log" >&2
     die "the named test is ALREADY failing before the mutation — a probe against a red test proves nothing" ;;
esac

printf '   [3/6] mutate ... '
if ! eval "$MUTATE"; then
  printf 'FAILED\n'
  die "the --mutate command exited non-zero"
fi
changed=0
i=0
for f in "${FILES[@]}"; do
  cmp -s "$f" "$BACKUP_DIR/$i" || changed=1
  i=$((i + 1))
done
if [ "$changed" -eq 0 ]; then
  printf 'NO-OP\n'
  die "the mutation left every --file byte-identical (anchor matched zero occurrences) — a no-op mutation always 'survives'"
fi
printf 'ok\n'

printf '   [4/6] mutant build ... '
if ! build; then
  printf 'REJECTED\n'
  tail -30 "$BACKUP_DIR/build.log" >&2
  restore
  build > /dev/null 2>&1
  die "the mutant does not compile — a compiler-rejected mutant is NOT a kill; pick a mutation the compiler accepts"
fi
printf 'ok\n'

printf '   [5/6] mutant test ... '
run_test
probe_status=$?
if [ "$probe_status" -eq 2 ]; then
  exit 2
fi
if [ "$probe_status" -eq 0 ]; then
  printf 'SURVIVOR\n'
  verdict="SURVIVOR"
else
  printf 'killed\n'
  verdict="killed"
fi

printf '   [6/6] restore + rebuild + re-test ... '
restore
if ! build; then
  printf 'FAILED\n'
  tail -30 "$BACKUP_DIR/build.log" >&2
  die "the RESTORED tree does not build — the restore did not take"
fi
run_test
case $? in
  0) printf 'green\n' ;;
  2) exit 2 ;;
  *) printf 'STILL RED\n'; tail -40 "$BACKUP_DIR/ctest.log" >&2
     die "the named test is still failing after restore — the rebuild did not pick the restore up (see trap 1) or the restore is incomplete" ;;
esac

printf '== break-probe: %s -> %s\n' "$LABEL" "$verdict"
if [ "$verdict" = "SURVIVOR" ]; then
  exit 1
fi
exit 0
