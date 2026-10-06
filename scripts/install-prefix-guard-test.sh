#!/usr/bin/env bash
# Prefix-guard fixtures (plan 1122, task rel-prefix-guard). install.sh and
# `install.sh --uninstall` both call planar_prefix_guard (scripts/install-lib/
# prefix-guard.sh) before anything is removed, created or built:
#   - a root that is the empty string, /, or resolves to $HOME exits 2, even
#     with --force, naming the path and the rule, leaving the tree untouched;
#   - a root carrying the stamp, an executable bin/planar, planar.db or a
#     validated recovery journal is adopted without --force;
#   - any other non-empty root is refused (exit 1) unless --force adopts it.
# Every run is against a scratch copy of the installer, a scratch HOME and a stub
# cmake, so nothing is built and the operator's ~/.planar is never looked at.
# A shim `rm`, `mv` and `rmdir` ahead on PATH records every call and fails it,
# so a refusal that did not happen before removal is caught as a log entry (and
# a broken guard can never reach a real recursive delete of a root such as /).
# Runs under stock bash 3.2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-prefix-guard-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); }

REPO="$TMP/repo"
mkdir -p "$REPO/skills"
cp "$ROOT/install.sh" "$REPO/install.sh"
cp -R "$ROOT/scripts" "$REPO/scripts"
cp -R "$ROOT/agents" "$REPO/agents"
cp -R "$ROOT/skills/planar" "$REPO/skills/planar"
cp "$ROOT/install-cleanup.txt" "$REPO/install-cleanup.txt"
: > "$REPO/CMakeLists.txt"
: > "$REPO/CMakePresets.json"
perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$REPO/install.sh"

STUBS="$TMP/stubs"
mkdir -p "$STUBS"
cat > "$STUBS/cmake" <<'STUB'
#!/usr/bin/env bash
if [[ "$1" == "--install" ]]; then
  prefix="$4"
  mkdir -p "$prefix/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    # `queue status 1 --json` answers a status object, so the install's queue
    # store probe of a preserved planar.db reads "usable".
    printf '#!/bin/sh\n[ "$1" = version ] && echo "planar guard-test"\n[ "$1" = queue ] && echo "{\\"seq\\":1}"\nexit 0\n' > "$prefix/bin/$b"
    chmod +x "$prefix/bin/$b"
  done
fi
exit 0
STUB
chmod +x "$STUBS/cmake"

SHIM="$TMP/shim"
SHIMLOG="$TMP/shim.log"
mkdir -p "$SHIM"
for c in rm mv rmdir; do
  cat > "$SHIM/$c" <<STUB
#!/bin/sh
printf '%s %s\n' "$c" "\$*" >> "$SHIMLOG"
exit 99
STUB
  chmod +x "$SHIM/$c"
done

# snapshot DIR -- listing and checksums, for before/after comparison.
snapshot() {
  ( cd "$1" && find . -print | sort && find . -type f -exec cksum {} + | sort )
}

# run_guarded HOME PREFIX_ARGS... -- run install.sh with the shim ahead on PATH
# (removals are recorded and refused). Sets RC; stdout/stderr in $TMP/{out,err}.
# A literal "--" ends HOME's args; the rest go to install.sh.
run_installer() {
  local home="$1" shimmed="$2"; shift 2
  local path="$STUBS:$PATH"
  [[ "$shimmed" == 1 ]] && path="$SHIM:$path"
  : > "$SHIMLOG"
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME PATH="$path" HOME="$home" PLANAR_DB="$home/planar.db" NO_COLOR=1 \
    ${RUN_PLANAR_HOME+PLANAR_HOME="$RUN_PLANAR_HOME"} \
    "$REPO/install.sh" --build-dir "$home/build" --no-vendor "$@" >"$TMP/out" 2>"$TMP/err" || RC=$?
}

new_home() {
  local h="$TMP/$1/user"
  mkdir -p "$h/bin" "$h/scripts"
  printf 'keep\n' > "$h/bin/tool"
  printf 'keep\n' > "$h/scripts/run.sh"
  printf '%s\n' "$h"
}

# --- refusals: $HOME, /, the empty string -----------------------------------

# expect_hard_refusal LABEL HOME ROOT FORCE OPERATION RULE-TEXT
expect_hard_refusal() {
  local label="$1" home="$2" root="$3" force="$4" op="$5" want="$6" before after flags=()
  [[ "$force" == 1 ]] && flags+=(--force)
  [[ "$op" == uninstall ]] && flags+=(--uninstall)
  before="$(snapshot "$home")"
  RUN_PLANAR_HOME="$home/.planar-default"
  run_installer "$home" 1 --prefix "$root" ${flags[@]+"${flags[@]}"}
  [[ "$RC" == 2 ]] || fail "$label ($op force=$force): expected exit 2, got $RC: $(cat "$TMP/err")"
  grep -Fq "$want" "$TMP/err" || fail "$label ($op force=$force): stderr does not contain '$want': $(cat "$TMP/err")"
  grep -Fq "Rule:" "$TMP/err" || fail "$label ($op force=$force): stderr does not name the rule: $(cat "$TMP/err")"
  grep -Fq "does not override" "$TMP/err" || fail "$label ($op force=$force): stderr does not say --force is no override"
  [[ ! -s "$SHIMLOG" ]] || fail "$label ($op force=$force): a removal was attempted before the refusal: $(cat "$SHIMLOG")"
  [[ ! -e "$home/build" && ! -e "$home/.planar-default" ]] || fail "$label ($op force=$force): the installer created something"
  after="$(snapshot "$home")"
  [[ "$before" == "$after" ]] || fail "$label ($op force=$force): the arena changed"
  unset RUN_PLANAR_HOME
  pass
}

H="$(new_home refuse)"
mkdir -p "$TMP/refuse/links"
ln -s "$H" "$TMP/refuse/links/to-home"
ln -s / "$TMP/refuse/links/to-root"
mkdir -p "$TMP/refuse/links/mid"
ln -s "$H" "$TMP/refuse/links/mid/inner"

UPS="$H$(printf '/..%.0s' $(seq 1 40))"
for op in install uninstall; do
  for force in 0 1; do
    expect_hard_refusal "home" "$H" "$H" "$force" "$op" "$H"
    expect_hard_refusal "home trailing slash" "$H" "$H/" "$force" "$op" "$H/"
    expect_hard_refusal "home dot" "$H" "$H/." "$force" "$op" "$H/."
    expect_hard_refusal "home dotdot" "$H" "$H/../user" "$force" "$op" "$H/../user"
    expect_hard_refusal "home via symlink" "$H" "$TMP/refuse/links/to-home" "$force" "$op" "$TMP/refuse/links/to-home"
    expect_hard_refusal "home via chained symlink" "$H" "$TMP/refuse/links/mid/inner/." "$force" "$op" "inner"
    expect_hard_refusal "root" "$H" "/" "$force" "$op" "/"
    expect_hard_refusal "root dotdot" "$H" "$UPS" "$force" "$op" "/.."
    expect_hard_refusal "root via symlink" "$H" "$TMP/refuse/links/to-root" "$force" "$op" "$TMP/refuse/links/to-root"
    expect_hard_refusal "empty --prefix" "$H" "" "$force" "$op" "empty string"
  done
done

# PLANAR_HOME set to the empty string (no --prefix) is the empty string too.
for op in install uninstall; do
  for force in 0 1; do
    flags=()
    [[ "$force" == 1 ]] && flags+=(--force)
    [[ "$op" == uninstall ]] && flags+=(--uninstall)
    before="$(snapshot "$H")"
    RUN_PLANAR_HOME=""
    run_installer "$H" 1 ${flags[@]+"${flags[@]}"}
    unset RUN_PLANAR_HOME
    [[ "$RC" == 2 ]] || fail "PLANAR_HOME= ($op force=$force): expected exit 2, got $RC: $(cat "$TMP/err")"
    grep -Fq "empty string" "$TMP/err" || fail "PLANAR_HOME= ($op force=$force): the rule is not named"
    [[ ! -s "$SHIMLOG" ]] || fail "PLANAR_HOME= ($op force=$force): a removal was attempted"
    [[ "$before" == "$(snapshot "$H")" ]] || fail "PLANAR_HOME= ($op force=$force): the arena changed"
    pass
  done
done

# PLANAR_HOME naming $HOME (environment, no --prefix) is refused as well.
before="$(snapshot "$H")"
RUN_PLANAR_HOME="$H"
run_installer "$H" 1 --force --uninstall
unset RUN_PLANAR_HOME
[[ "$RC" == 2 ]] || fail "PLANAR_HOME=\$HOME --uninstall --force: expected exit 2, got $RC"
[[ "$before" == "$(snapshot "$H")" && ! -s "$SHIMLOG" ]] || fail "PLANAR_HOME=\$HOME --uninstall --force touched the arena"
pass

# A case variant of $HOME names the same directory on a case-insensitive volume
# (the macOS default). The canonical string differs, so only identity (-ef)
# catches it. Skipped, with the reason printed, when the scratch volume is
# case-sensitive.
HV="$(dirname "$H")/$(basename "$H" | tr '[:lower:]' '[:upper:]')"
if [[ "$HV" != "$H" && -d "$HV" ]]; then
  for op in install uninstall; do
    for force in 0 1; do
      expect_hard_refusal "home case variant" "$H" "$HV" "$force" "$op" "$HV"
    done
  done
else
  printf 'install-prefix-guard-test: skipping case-variant HOME fixtures: the scratch filesystem is case-sensitive\n'
fi

# An alias the canonical string cannot see: the same directory under a second
# spelling. Drives the guard function with a resolver that does not normalise
# the alias, so only the device+inode comparison can refuse it.
(
  # shellcheck disable=SC1091
  source "$ROOT/scripts/install-lib/prefix-guard.sh"
  # shellcheck disable=SC2329  # called by planar_prefix_guard
  planar_canonical_path() {
    case "$1" in
      alias-of-home) printf '%s\n' "$H/." ;;
      alias-of-root) printf '%s\n' "//" ;;
      *) printf '%s\n' "$1" ;;
    esac
  }
  HOME="$H"
  for force in 0 1; do
    rc=0; planar_prefix_guard alias-of-home "$force" install 2>/dev/null || rc=$?
    [[ "$rc" == 2 ]] || exit 21
    rc=0; planar_prefix_guard alias-of-home "$force" uninstall 2>/dev/null || rc=$?
    [[ "$rc" == 2 ]] || exit 22
    rc=0; planar_prefix_guard alias-of-root "$force" uninstall 2>/dev/null || rc=$?
    [[ "$rc" == 2 ]] || exit 23
  done
) || fail "identity (-ef) alias check $? failed"
pass

# --- legacy installs are adopted without --force ------------------------------

stamp_of() { cat "$1/.planar-install" 2>/dev/null || true; }

# expect_adopted LABEL PREFIX: an install without --force completes and stamps.
expect_adopted() {
  local label="$1" prefix="$2" home
  home="$(dirname "$prefix")"
  run_installer "$home" 0 --prefix "$prefix"
  [[ "$RC" == 0 ]] || fail "$label: expected adoption (exit 0), got $RC: $(cat "$TMP/err")"
  [[ -f "$prefix/.planar-install" ]] || fail "$label: no stamp after install"
  grep -Fq 'planar-install' "$prefix/.planar-install" || fail "$label: malformed stamp"
  [[ -x "$prefix/bin/planar" ]] || fail "$label: no bin/planar after install"
  pass
}

# Executable bin/planar and no stamp (an install made before the stamp existed).
mkdir -p "$TMP/legacy-bin/.planar/bin"
printf '#!/bin/sh\nexit 0\n' > "$TMP/legacy-bin/.planar/bin/planar"
chmod +x "$TMP/legacy-bin/.planar/bin/planar"
printf 'mine\n' > "$TMP/legacy-bin/.planar/notes.txt"
expect_adopted "legacy bin/planar" "$TMP/legacy-bin/.planar"

# Only planar.db (a preserved database).
mkdir -p "$TMP/legacy-db/.planar"
printf 'SQLite format 3\n' > "$TMP/legacy-db/.planar/planar.db"
expect_adopted "legacy planar.db" "$TMP/legacy-db/.planar"
[[ "$(cat "$TMP/legacy-db/.planar/planar.db")" == "SQLite format 3" ]] || fail "adoption touched planar.db"

# The stamp alone.
mkdir -p "$TMP/legacy-stamp/.planar"
printf 'planar-install 0.9\nbuild old\n' > "$TMP/legacy-stamp/.planar/.planar-install"
expect_adopted "stamp only" "$TMP/legacy-stamp/.planar"

# A prefix that does not exist yet and an empty one are fresh installs.
mkdir -p "$TMP/fresh"
expect_adopted "absent prefix" "$TMP/fresh/.planar"
mkdir -p "$TMP/fresh-empty/.planar"
expect_adopted "empty prefix" "$TMP/fresh-empty/.planar"

# --- foreign roots: refused naming the path; --force adopts -------------------

make_foreign() {
  local d="$TMP/$1/.planar"
  mkdir -p "$d/staging-notes"
  printf 'precious\n' > "$d/thesis.txt"
  printf '%s\n' "$d"
}

# expect_ownership_refusal LABEL PREFIX OPERATION
expect_ownership_refusal() {
  local label="$1" prefix="$2" op="$3" before flags=()
  [[ "$op" == uninstall ]] && flags+=(--uninstall)
  before="$(snapshot "$prefix")"
  run_installer "$(dirname "$prefix")" 1 --prefix "$prefix" ${flags[@]+"${flags[@]}"}
  [[ "$RC" == 1 ]] || fail "$label ($op): expected exit 1, got $RC: $(cat "$TMP/err")"
  grep -Fq "$prefix" "$TMP/err" || fail "$label ($op): stderr does not name $prefix"
  grep -Fq "does not look like a Planar install" "$TMP/err" || fail "$label ($op): the rule is not named"
  [[ ! -s "$SHIMLOG" ]] || fail "$label ($op): a removal was attempted: $(cat "$SHIMLOG")"
  [[ "$before" == "$(snapshot "$prefix")" ]] || fail "$label ($op): the prefix changed"
  pass
}

F="$(make_foreign foreign)"
expect_ownership_refusal "foreign root" "$F" install
expect_ownership_refusal "foreign root" "$F" uninstall

# --force adopts it, for install and for uninstall.
run_installer "$(dirname "$F")" 0 --prefix "$F" --force
[[ "$RC" == 0 && -f "$F/.planar-install" ]] || fail "--force did not adopt the foreign root: rc=$RC $(cat "$TMP/err")"
pass
F2="$(make_foreign foreign2)"
run_installer "$(dirname "$F2")" 0 --prefix "$F2" --force --uninstall
[[ "$RC" == 0 && ! -e "$F2" ]] || fail "--force --uninstall did not remove the foreign root: rc=$RC $(cat "$TMP/err")"
pass

# A planar-owned root is uninstalled without --force.
mkdir -p "$TMP/own/.planar/bin"
printf '#!/bin/sh\nexit 0\n' > "$TMP/own/.planar/bin/planar"
chmod +x "$TMP/own/.planar/bin/planar"
printf 'x\n' > "$TMP/own/.planar/other.txt"
run_installer "$TMP/own" 0 --prefix "$TMP/own/.planar" --uninstall
[[ "$RC" == 0 ]] || fail "uninstall of an owned root failed: rc=$RC $(cat "$TMP/err")"
[[ ! -e "$TMP/own/.planar/bin" && ! -e "$TMP/own/.planar/other.txt" ]] || fail "uninstall of an owned root left files"
pass

# --- recovery journal ----------------------------------------------------------

# write_journal PREFIX LINES... (one argument per line)
write_journal() {
  local prefix="$1"; shift
  mkdir -p "$prefix"
  : > "$prefix/.planar-journal"
  local l
  for l in "$@"; do printf '%s\n' "$l" >> "$prefix/.planar-journal"; done
}

# A validated journal for each phase adopts an otherwise unowned root.
for phase in prepared mutating complete aborted-before-mutation uninstalling; do
  P="$TMP/jvalid-$phase/.planar"
  write_journal "$P" "planar-journal 1" "root=$P" "phase=$phase" "owner=ignored-extra-key"
  printf 'partial\n' > "$P/.staging-bin"
  # the uninstalling phase is exercised by the install path only here
  expect_adopted "valid journal ($phase)" "$P"
done

# Forged and unrelated markers never adopt.
reject_journal() {
  local label="$1" P="$TMP/jbad-$1/.planar"; shift
  mkdir -p "$P"
  printf 'x\n' > "$P/thing"
  "$@" "$P"
  expect_ownership_refusal "journal: $label" "$P" install
  expect_ownership_refusal "journal: $label" "$P" uninstall
}
j_wrong_root()   { write_journal "$1" "planar-journal 1" "root=/somewhere/else" "phase=mutating"; }
j_wrong_ver()    { write_journal "$1" "planar-journal 2" "root=$1" "phase=mutating"; }
j_no_marker()    { write_journal "$1" "root=$1" "phase=mutating"; }
j_bad_phase()    { write_journal "$1" "planar-journal 1" "root=$1" "phase=whenever"; }
j_no_phase()     { write_journal "$1" "planar-journal 1" "root=$1"; }
j_no_root()      { write_journal "$1" "planar-journal 1" "phase=mutating"; }
j_dup_key()      { write_journal "$1" "planar-journal 1" "root=$1" "phase=mutating" "phase=complete"; }
j_malformed()    { write_journal "$1" "planar-journal 1" "root=$1" "phase=mutating" "this line is not key=value"; }
j_bad_key()      { write_journal "$1" "planar-journal 1" "root=$1" "phase=mutating" "Bad-Key=1"; }
j_empty()        { : > "$1/.planar-journal"; }
j_bare_staging() { mkdir -p "$1/.staging-123" "$1/bin.old"; rm -f "$1/thing"; printf 'x\n' > "$1/.staging-123/f"; }
j_symlink() {
  write_journal "$TMP/jsrc" "planar-journal 1" "root=$1" "phase=mutating"
  ln -s "$TMP/jsrc/.planar-journal" "$1/.planar-journal"
}
j_dir()          { mkdir -p "$1/.planar-journal"; }
j_nonprint()     { printf 'planar-journal 1\nroot=%s\nphase=mutating\n\001\n' "$1" > "$1/.planar-journal"; }
j_oversize() {
  write_journal "$1" "planar-journal 1" "root=$1" "phase=mutating"
  local i
  for i in 1 2 3 4 5 6 7 8; do
    printf 'pad%s=%s\n' "$i" "$(head -c 9000 /dev/zero | tr '\0' a)" >> "$1/.planar-journal"
  done
}
j_other_stamp_name() { printf 'planar-install 1\n' > "$1/.planar-install.tmp"; }
for c in j_wrong_root j_wrong_ver j_no_marker j_bad_phase j_no_phase j_no_root j_dup_key j_malformed \
         j_bad_key j_empty j_bare_staging j_symlink j_dir j_nonprint j_oversize j_other_stamp_name; do
  reject_journal "$c" "$c"
done

# A journal for another root copied into this one is forged evidence.
P="$TMP/jcopy/.planar"; Q="$TMP/jcopy-src/.planar"
write_journal "$Q" "planar-journal 1" "root=$Q" "phase=mutating"
mkdir -p "$P"; cp "$Q/.planar-journal" "$P/.planar-journal"
expect_ownership_refusal "journal copied from another root" "$P" install
# --force still adopts a root whose journal is not valid.
run_installer "$(dirname "$P")" 0 --prefix "$P" --force
[[ "$RC" == 0 && -f "$P/.planar-install" ]] || fail "--force did not adopt a forged-journal root"
pass

# A journal reached through a symlinked prefix names the canonical root.
mkdir -p "$TMP/jlink-real/.planar"
write_journal "$TMP/jlink-real/.planar" "planar-journal 1" "root=$TMP/jlink-real/.planar" "phase=prepared"
mkdir -p "$TMP/jlink"
ln -s "$TMP/jlink-real/.planar" "$TMP/jlink/.planar"
expect_adopted "valid journal through a symlinked prefix" "$TMP/jlink/.planar"

# --- canonicalisation unit checks ----------------------------------------------

(
  # shellcheck disable=SC1091
  source "$ROOT/scripts/install-lib/prefix-guard.sh"
  mkdir -p "$TMP/canon/real"
  ln -s real "$TMP/canon/rel-link"
  ln -s "$TMP/canon/real" "$TMP/canon/abs-link"
  ln -s abs-link "$TMP/canon/chain"
  [[ "$(planar_canonical_path "$TMP/canon/rel-link/x/../y")" == "$TMP/canon/real/y" ]] || exit 11
  [[ "$(planar_canonical_path "$TMP/canon/chain/")" == "$TMP/canon/real" ]] || exit 12
  [[ "$(planar_canonical_path "$TMP/canon/not/yet/there/..")" == "$TMP/canon/not/yet" ]] || exit 13
  [[ "$(planar_canonical_path "/")" == "/" ]] || exit 14
  [[ "$(planar_canonical_path "/..")" == "/" ]] || exit 15
  [[ "$(cd "$TMP/canon" && planar_canonical_path "real/../rel-link")" == "$TMP/canon/real" ]] || exit 16
  ln -s loop-b "$TMP/canon/loop-a"; ln -s loop-a "$TMP/canon/loop-b"
  ! planar_canonical_path "$TMP/canon/loop-a" >/dev/null 2>&1 || exit 17
  ! planar_canonical_path "" >/dev/null 2>&1 || exit 18
) || fail "planar_canonical_path unit check $? failed"
pass

printf 'install prefix guard tests: %s passed\n' "$PASSED"
