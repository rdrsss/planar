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
#   5. THE ZERO-EFFECT TRAP — the dangerous one (task 6336). Trap 2 proves
#      the mutation changed the FILE. It does NOT prove the mutation changed
#      the BEHAVIOUR. A `perl s///` that lands on a COMMENT (or on
#      whitespace, or on dead code, or inside an `#if 0`) changes the file,
#      compiles to a semantically identical binary, and the named test of
#      course still passes — so the script reported SURVIVOR. That is a
#      finding about NOTHING, and it is worse than a false alarm: read the
#      other way, an inert mutation that a coder accepts as an "equivalent
#      mutant" hands the test a certificate it never earned. Break-probing
#      is this port's evidence standard; a gap in the prover outranks a gap
#      in any one proof. This script therefore hashes the behaviour-bearing
#      SECTIONS of every linked binary (code, string literals, initialised
#      data) before and after the mutant build. If not one of those bytes
#      changed, the mutation was INERT and the script says so explicitly —
#      a distinct verdict, never SURVIVOR.
#
#      Getting the discriminator right took three tries; the two that
#      failed are recorded here because both looked obviously correct
#      (task 6336, measured on this toolchain):
#
#        - Hashing the .o files does NOT work. A module interface unit's
#          object embeds the BMI, which carries the source text, so editing
#          a COMMENT in a .cppm changes its .o. The check would have called
#          the exact mutation it exists to catch "behavioural".
#        - Hashing the linked executable does not work either, with or
#          without llvm-strip: even a fully stripped Mach-O still differed
#          after a comment-only edit.
#        - Dumping just the loadable sections DOES work: across a
#          comment-only edit, every byte of __text/__cstring/__data was
#          identical, while a one-line behavioural edit moved thousands of
#          them. That is the rule implemented below.
#
#      The check is conservative by construction — it claims INERT only on
#      byte-identical sections, so any residual nondeterminism can only
#      cost a missed INERT (degrading to the old SURVIVOR report), never a
#      false INERT. And it refuses to run at all if it dumps no sections,
#      so the discriminator cannot silently match nothing — which is trap 2
#      applied to this check itself.
#
#   6. THE UNVERIFIED RESTORE — task 6344. A prior cycle reported "working
#      tree clean, zero residue" after its probes, while a live, compiled,
#      uncommitted mutant sat in the worktree at a file this script had
#      "restored". The restore step had run — `cp` from the backup — but
#      nothing checked it actually took, and the report asserted cleanliness
#      instead of showing it. Compounding factor: an interrupted run (an
#      agent stopped mid-gate, a killed background job) hits the EXIT/INT/
#      TERM trap's restore path, which is exactly the path nobody was
#      checking. This script now hashes every file's pre-mutation backup,
#      re-hashes it after every restore (both the normal completion path
#      and the trap), and refuses to report success on a digest mismatch —
#      loudly, on stderr, naming the exact file and the fix-it-by-hand
#      command. It also prints `git status --short` for the probed files on
#      every restore, so a caller sees the tree state directly rather than
#      trusting an assertion.
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
#   baseline green -> mutant builds -> mutant CHANGED THE OBJECTS ->
#   named test FAILS -> restored -> rebuilt -> named test GREEN again.
#
#   0  killed   — the mutation changed codegen and the named test caught it.
#   1  SURVIVOR — the mutation changed codegen and the named test did NOT
#                 catch it. A finding about the TEST, not about this script.
#   3  INERT    — the mutation changed the file but not one compiled byte.
#                 A finding about the MUTATION: it proves nothing either
#                 way, so re-aim it at code that actually runs and re-probe.
#   2  anything else — a diagnostic naming which step broke.
#
# INERT is deliberately NOT folded into SURVIVOR. The two look identical
# from the test's point of view (mutant builds, test stays green) and mean
# opposite things: a survivor indicts the test, an inert mutant indicts the
# probe. Reporting the second as the first is how a test acquires evidence
# it never earned.

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

# Pick a checksum tool up front — restore-verification (task 6344) needs it
# on every exit path, including the trap, so it cannot wait for the later
# "pick a checksum tool" step below.
if command -v shasum > /dev/null 2>&1; then
  RESTORE_SHA_TOOL=(shasum -a 256)
elif command -v sha256sum > /dev/null 2>&1; then
  RESTORE_SHA_TOOL=(sha256sum)
else
  die "neither shasum nor sha256sum is available — cannot verify restores (task 6344)"
fi

digest_of() {
  "${RESTORE_SHA_TOOL[@]}" < "$1" | cut -d' ' -f1
}

# Restore by CONTENT into the original path and then bump the mtime, rather
# than `mv`-ing the backup over it: see trap 1 above.
#
# task 6344: a prior cycle reported "working tree clean, zero residue" while
# a live, compiled, uncommitted mutant sat in the tree — the restore step ran
# but nobody checked it actually took. This function now VERIFIES every
# restored file's digest against the pre-mutation backup's digest before
# declaring success, and refuses to lie: on any mismatch it prints the exact
# path loudly to stderr and returns failure instead of silently continuing.
# This runs on EVERY exit path (normal completion AND the EXIT/INT/TERM trap
# below), so an interrupted run cannot leave a live mutant behind unreported.
restore() {
  [ "$RESTORED" -eq 1 ] && return 0
  local i=0 f rc=0
  for f in "${FILES[@]}"; do
    if ! cp "$BACKUP_DIR/$i" "$f"; then
      printf 'break-probe: RESTORE FAILED for %s — could not copy backup into place. THE TREE IS MUTATED. Fix it by hand:\n  cp %q %q\n' \
        "$f" "$BACKUP_DIR/$i" "$f" >&2
      rc=1
      i=$((i + 1))
      continue
    fi
    touch "$f"
    local want got
    want="$(digest_of "$BACKUP_DIR/$i")"
    got="$(digest_of "$f")"
    if [ "$want" != "$got" ]; then
      printf 'break-probe: RESTORE VERIFICATION FAILED for %s\n' "$f" >&2
      printf '  expected sha256 %s (pre-mutation backup)\n' "$want" >&2
      printf '  found    sha256 %s (after restore)\n' "$got" >&2
      printf '  THE TREE MAY STILL BE MUTATED. Do not trust any gate result from this run.\n' >&2
      printf '  Restore by hand:\n  cp %q %q\n' "$BACKUP_DIR/$i" "$f" >&2
      rc=1
    fi
    i=$((i + 1))
  done
  # Loud, unconditional evidence for whatever invoked this script — task
  # 6344 item 3: don't make callers trust an assertion of cleanliness, show
  # them the actual git state of every file this probe touched.
  if command -v git > /dev/null 2>&1 && git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    printf 'break-probe: post-restore git status (should be empty for a clean restore):\n' >&2
    git status --short -- "${FILES[@]}" >&2
  fi
  if [ "$rc" -eq 0 ]; then
    printf 'break-probe: restore verified (digest match on all %d file(s)).\n' "${#FILES[@]}" >&2
  else
    printf 'break-probe: RESTORE NOT VERIFIED — see above. This is not a probe verdict, it is a broken tree.\n' >&2
  fi
  RESTORED=1
  return "$rc"
}

cleanup() {
  restore || printf 'break-probe: cleanup restore failed — inspect the tree before trusting anything else in it.\n' >&2
  rm -rf "$BACKUP_DIR"
}
# A single `trap cleanup EXIT INT TERM` looked right but was live-fire tested
# (task 6344) and found broken: bash's default post-trap behavior for INT/TERM
# on a non-interactive script is to RESUME execution after the handler
# returns, not to terminate. An interruption mid-run therefore ran cleanup()
# (which restores AND `rm -rf`s $BACKUP_DIR), then kept executing the probe
# body against a backup directory that no longer existed — every subsequent
# reference to $BACKUP_DIR failed with "No such file or directory" and the
# script limped on printing garbage instead of stopping. INT/TERM must each
# `exit` explicitly after cleanup so the process actually terminates; that
# exit re-fires the EXIT trap, which is safe — `restore` no-ops via its
# RESTORED guard and `rm -rf` on an already-gone directory is a no-op.
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

# Pick a checksum tool once. macOS ships `shasum`, most Linuxes ship
# `sha256sum`; either is fine, we only ever compare our own output against
# our own output from the same run.
if command -v shasum > /dev/null 2>&1; then
  SHA_TOOL=(shasum -a 256)
elif command -v sha256sum > /dev/null 2>&1; then
  SHA_TOOL=(sha256sum)
else
  die "neither shasum nor sha256sum is available — cannot run the zero-effect check (trap 5)"
fi

# Locate llvm-objcopy, preferring the toolchain this build directory was
# actually configured with (CMakePresets pins a specific LLVM; see
# docs/toolchain-parity.md) so the section dump matches the linker's output.
find_objcopy() {
  local cxx dir cand
  cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt" 2> /dev/null | head -1)"
  if [ -n "$cxx" ]; then
    dir="$(dirname "$cxx")"
    for cand in "$dir/llvm-objcopy" "$dir/objcopy"; do
      [ -x "$cand" ] && { printf '%s\n' "$cand"; return 0; }
    done
  fi
  for cand in llvm-objcopy objcopy; do
    command -v "$cand" > /dev/null 2>&1 && { command -v "$cand"; return 0; }
  done
  return 1
}

OBJCOPY="$(find_objcopy)" || die "neither llvm-objcopy nor objcopy is available — cannot run the zero-effect check (trap 5)"

# The sections that carry behaviour: code, string literals, and initialised
# data. Deliberately NOT the debug or symbol sections — those record where
# code came from, not what it does, and they change when a COMMENT changes
# (verified on this toolchain, task 6336: a comment-only edit to a .cppm
# altered the module object, the linked executable, and even the fully
# stripped executable, but left every byte of __text identical). Both Mach-O
# and ELF spellings are listed; a section that does not exist dumps nothing
# and is skipped.
SECTIONS=(
  "__TEXT,__text" "__TEXT,__cstring" "__TEXT,__const"
  "__DATA,__data" "__DATA_CONST,__const"
  ".text" ".rodata" ".data"
)

# List the linked binaries ctest can run. CMakeFiles/ is excluded: it holds
# configure-time compiler probes, not build output.
list_binaries() {
  find "$BUILD_DIR" -type f -perm -u+x \
    ! -name '*.o' ! -name '*.a' ! -name '*.dylib' ! -name '*.so' \
    ! -path '*/CMakeFiles/*' \
    | sort
}

# Hash the behaviour-bearing sections of every linked binary into the file
# named by $1. Returns non-zero if it produced nothing at all, which the
# caller must treat as a broken discriminator rather than as "no change".
hash_code() {
  local out="$1" tmp exe sec n=0
  tmp="$BACKUP_DIR/section.bin"
  : > "$out"
  while IFS= read -r exe; do
    for sec in "${SECTIONS[@]}"; do
      rm -f "$tmp"
      "$OBJCOPY" --dump-section "$sec=$tmp" "$exe" /dev/null > /dev/null 2>&1
      [ -s "$tmp" ] || continue
      printf '%s  %s::%s\n' \
        "$("${SHA_TOOL[@]}" < "$tmp" | cut -d' ' -f1)" \
        "${exe#"$BUILD_DIR"/}" "$sec" >> "$out"
      n=$((n + 1))
    done
  done < <(list_binaries)
  rm -f "$tmp"
  [ "$n" -gt 0 ]
}

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

printf '   [1/7] baseline build ... '
if ! build; then
  printf 'FAILED\n'
  tail -30 "$BACKUP_DIR/build.log" >&2
  die "the UNMUTATED tree does not build — nothing this script reports afterwards would mean anything"
fi
# Snapshot codegen for the zero-effect check (trap 5). An empty manifest
# means the discriminator matches nothing, which would make every mutation
# look inert — refuse rather than report a verdict we cannot support.
if ! hash_code "$BACKUP_DIR/code.base"; then
  printf 'ok\n'
  die "dumped ZERO code sections from the binaries under '$BUILD_DIR' — the zero-effect check (trap 5) would then match nothing and call every mutation inert; a check that silently matches nothing is exactly the failure it exists to catch"
fi
sec_count="$(grep -c '' < "$BACKUP_DIR/code.base")"
printf 'ok (%s code sections)\n' "$sec_count"

printf '   [2/7] baseline test ... '
run_test
case $? in
  0) printf 'green\n' ;;
  2) exit 2 ;;
  *) printf 'FAILED\n'; tail -40 "$BACKUP_DIR/ctest.log" >&2
     die "the named test is ALREADY failing before the mutation — a probe against a red test proves nothing" ;;
esac

printf '   [3/7] mutate ... '
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

printf '   [4/7] mutant build ... '
if ! build; then
  printf 'REJECTED\n'
  tail -30 "$BACKUP_DIR/build.log" >&2
  restore || die "the mutant does not compile, AND the restore did not verify — the tree is left MUTATED; see the digest mismatch above and fix it by hand before trusting anything else"
  build > /dev/null 2>&1
  die "the mutant does not compile — a compiler-rejected mutant is NOT a kill; pick a mutation the compiler accepts"
fi
printf 'ok\n'

# Trap 5: did the mutation change any compiled byte? A mutation that
# changed the file but not the codegen (a comment, whitespace, dead code)
# cannot be caught by ANY test, so running the test would only produce a
# meaningless green. Decide this before the test, not after.
printf '   [5/7] zero-effect check ... '
hash_code "$BACKUP_DIR/code.mutant" || die "dumped zero code sections after the mutant build — the discriminator broke mid-probe"
if cmp -s "$BACKUP_DIR/code.base" "$BACKUP_DIR/code.mutant"; then
  printf 'INERT\n'
  verdict="INERT"
else
  changed_secs="$(diff "$BACKUP_DIR/code.base" "$BACKUP_DIR/code.mutant" | grep -c '^>')"
  printf 'ok (%s code sections changed)\n' "$changed_secs"
  verdict=""
fi

if [ -z "$verdict" ]; then
  printf '   [6/7] mutant test ... '
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
else
  printf '   [6/7] mutant test ... SKIPPED (inert mutant; the test result would mean nothing)\n'
fi

printf '   [7/7] restore + rebuild + re-test ... '
restore || die "restore did not verify (digest mismatch) — the tree is left MUTATED; see above and fix it by hand before trusting any other result"
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
case "$verdict" in
  SURVIVOR) exit 1 ;;
  INERT)
    printf 'break-probe: the mutation changed the file but NOT one compiled byte.\n' >&2
    printf 'It landed on something that does not run — a comment, whitespace, dead code,\n' >&2
    printf 'or a branch nothing reaches. This is NOT a survivor and says nothing about the\n' >&2
    printf 'test: no test can catch a mutation that changes no code. Re-aim the mutation at\n' >&2
    printf 'a line that actually executes and probe again.\n' >&2
    exit 3
    ;;
esac
exit 0
