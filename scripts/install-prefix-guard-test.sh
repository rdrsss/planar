#!/usr/bin/env bash
# Prefix-guard fixtures (plan 1122, tasks rel-prefix-guard and
# rel-uninstall-script). install.sh and the standalone uninstaller
# (scripts/uninstall.sh, which `install.sh --uninstall` runs) both call
# planar_prefix_guard (scripts/install-lib/prefix-guard.sh) before anything is
# removed, created or built:
#   - a root that is the empty string, /, or resolves to $HOME exits 2, even
#     with --force, naming the path and the rule, leaving the tree untouched;
#   - a root carrying the stamp, an executable bin/planar, planar.db, a
#     validated recovery journal, a validated uninstalled marker, or (only in
#     $HOME/.planar, however it is spelled) a listed data path is adopted
#     without --force;
#   - any other non-empty root is refused (exit 1) unless --force adopts it;
#   - an uninstall has no --force: it exits 2 naming --purge.
# Every run is against a scratch copy of the installer, a scratch HOME and a stub
# cmake, so nothing is built and the operator's ~/.planar is never looked at.
# A shim `rm`, `mv` and `rmdir` ahead on PATH records every call and fails it,
# so a refusal that did not happen before removal is caught as a log entry (and
# a broken guard can never reach a real recursive delete of a root such as /).
# Runs under stock bash 3.2.
# shellcheck disable=SC2015,SC2016  # A && B || fail is intended; literal $HOME in messages
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-prefix-guard-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); }

REPO="$TMP/repo"
mkdir -p "$REPO/skills" "$REPO/workflows" "$REPO/migrations"
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
cat > "$STUBS/cmake" <<STUB
#!/usr/bin/env bash
# A stub cmake: \`cmake --install D --prefix P\` writes the five fake binaries of
# scripts/fixtures/prebuilt-bundle.sh (stub_binary_write) into P/bin.
if [[ "\$1" == "--install" ]]; then
  source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
  mkdir -p "\$4/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    stub_binary_write "\$4/bin/\$b" "\${STUB_TAG:-dev}"
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
  if [[ "$op" == uninstall && "$force" == 1 ]]; then
    # An uninstall has no --force at all: refused first, naming --purge.
    grep -Fq -- "--purge" "$TMP/err" || fail "$label ($op force=$force): the --force refusal does not name --purge: $(cat "$TMP/err")"
  else
    grep -Fq "$want" "$TMP/err" || fail "$label ($op force=$force): stderr does not contain '$want': $(cat "$TMP/err")"
    grep -Fq "Rule:" "$TMP/err" || fail "$label ($op force=$force): stderr does not name the rule: $(cat "$TMP/err")"
    grep -Fq "does not override" "$TMP/err" || fail "$label ($op force=$force): stderr does not say --force is no override"
  fi
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
    if [[ "$op" == uninstall && "$force" == 1 ]]; then
      grep -Fq -- "--purge" "$TMP/err" || fail "PLANAR_HOME= ($op force=$force): the --force refusal does not name --purge"
    else
      grep -Fq "empty string" "$TMP/err" || fail "PLANAR_HOME= ($op force=$force): the rule is not named"
    fi
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
  # shellcheck disable=SC2030  # the subshell is the point: HOME is local to it
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

# A root holding only preserved data paths (what an uninstall leaves behind when
# the database lives elsewhere) is adopted, install and uninstall, without
# --force, with or without unknown files beside it. Unknown files alone are not
# evidence (the foreign-root cases below).
for dp in workbench/x config.toml local/skills models workspaces templates execute retired queue-logs; do
  d="$TMP/dp-$(printf '%s' "$dp" | tr '/' '-')/.planar"
  mkdir -p "$d"
  case "$dp" in
    config.toml) printf '[x]\n' > "$d/config.toml" ;;
    *) mkdir -p "$d/$dp" ;;
  esac
  expect_adopted "data path $dp only" "$d"
done
d="$TMP/dp-mixed/.planar"
mkdir -p "$d/workbench"
printf 'mine\n' > "$d/thesis.txt"
expect_adopted "data path beside an unknown file" "$d"
[[ "$(cat "$d/thesis.txt")" == "mine" ]] || fail "adoption touched the unknown file"
d="$TMP/dp-uninstall/.planar"
mkdir -p "$d/workbench"
printf 'x\n' > "$d/workbench/doc.md"
run_installer "$(dirname "$d")" 0 --prefix "$d" --uninstall
[[ "$RC" == 0 ]] || fail "uninstall of a root holding only a data path was refused ($RC): $(cat "$TMP/err")"
[[ "$(cat "$d/workbench/doc.md")" == "x" ]] || fail "uninstall removed the preserved data path"
pass

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
# An uninstall has no --force, so a foreign root is never removed: refused at
# exit 2 naming --purge, the root untouched.
F2="$(make_foreign foreign2)"
before="$(snapshot "$F2")"
run_installer "$(dirname "$F2")" 1 --prefix "$F2" --force --uninstall
[[ "$RC" == 2 ]] || fail "--force --uninstall of a foreign root was not refused at exit 2: rc=$RC $(cat "$TMP/err")"
grep -Fq -- "--purge" "$TMP/err" || fail "--force --uninstall: the refusal does not name --purge: $(cat "$TMP/err")"
[[ ! -s "$SHIMLOG" && "$before" == "$(snapshot "$F2")" ]] || fail "--force --uninstall touched the foreign root"
pass

# A directory that merely holds a name from the data-path list is not a Planar
# root unless it is $HOME/.planar: a project with models/ or templates/ beside
# src/ and .git/ is refused, install and uninstall, and left byte for byte as it
# was (tech spec, "Prefix guard": arbitrary staging names do not qualify).
for dpname in models templates; do
  P="$TMP/lookalike-$dpname/proj"
  mkdir -p "$P/$dpname" "$P/src" "$P/.git"
  printf 'int main(){}\n' > "$P/src/main.c"
  printf 'ref: refs/heads/main\n' > "$P/.git/HEAD"
  printf 'catalog\n' > "$P/$dpname/x"
  expect_ownership_refusal "lookalike root holding $dpname/" "$P" install
  expect_ownership_refusal "lookalike root holding $dpname/" "$P" uninstall
done

# A planar-owned root is uninstalled without --force.
mkdir -p "$TMP/own/.planar/bin"
printf '#!/bin/sh\nexit 0\n' > "$TMP/own/.planar/bin/planar"
chmod +x "$TMP/own/.planar/bin/planar"
printf 'x\n' > "$TMP/own/.planar/other.txt"
run_installer "$TMP/own" 0 --prefix "$TMP/own/.planar" --uninstall
[[ "$RC" == 0 ]] || fail "uninstall of an owned root failed: rc=$RC $(cat "$TMP/err")"
[[ ! -e "$TMP/own/.planar/bin" ]] || fail "uninstall of an owned root left bin/"
# An entry the uninstaller does not know is kept and reported, never removed.
[[ "$(cat "$TMP/own/.planar/other.txt")" == x ]] || fail "uninstall removed the unknown other.txt"
grep -Fq "kept $TMP/own/.planar/other.txt" "$TMP/err" || fail "uninstall did not report the unknown entry it kept: $(cat "$TMP/err")"
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

# A validated journal for each phase passes the guard of an otherwise unowned
# root. Prepared, complete and aborted attempts leave nothing to resume, so the
# install completes; an unknown .staging-* entry is reported and kept.
for phase in prepared complete aborted-before-mutation; do
  P="$TMP/jvalid-$phase/.planar"
  write_journal "$P" "planar-journal 1" "root=$P" "phase=$phase" "owner=ignored-extra-key"
  printf 'partial\n' > "$P/.staging-bin"
  expect_adopted "valid journal ($phase)" "$P"
  [[ "$(cat "$P/.staging-bin")" == partial && ! -e "$P/.planar-journal" ]] || fail "valid journal ($phase): the unknown staging entry was not kept, or the journal was left"
done
# A mutating journal that records no target, and an uninstalling one, pass the
# guard too; the install state machine then refuses to replay them (the target
# is not this install's; an interrupted uninstall is never undone), keeping
# every file.
for phase in mutating uninstalling; do
  P="$TMP/jvalid-$phase/.planar"
  write_journal "$P" "planar-journal 1" "root=$P" "phase=$phase" "owner=ignored-extra-key"
  printf 'partial\n' > "$P/.staging-bin"
  before="$(snapshot "$P")"
  run_installer "$(dirname "$P")" 0 --prefix "$P"
  [[ "$RC" == 1 ]] || fail "valid journal ($phase): expected the state machine's refusal (exit 1), got $RC: $(cat "$TMP/err")"
  ! grep -Fq 'does not look like a Planar install' "$TMP/err" || fail "valid journal ($phase): the guard refused it: $(cat "$TMP/err")"
  case "$phase" in
    mutating) want='was interrupted after it changed the installation' ;;
    uninstalling) want='an uninstall of' ;;
  esac
  grep -Fq "$want" "$TMP/err" || fail "valid journal ($phase): the refusal does not say '$want': $(cat "$TMP/err")"
  [[ "$before" == "$(snapshot "$P")" ]] || fail "valid journal ($phase): the refusal changed the root"
  pass
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
j_del()          { printf 'planar-journal 1\nroot=%s\nphase=mutating\n\177\n' "$1" > "$1/.planar-journal"; }
j_oversize() {
  write_journal "$1" "planar-journal 1" "root=$1" "phase=mutating"
  local i
  for i in 1 2 3 4 5 6 7 8; do
    printf 'pad%s=%s\n' "$i" "$(head -c 9000 /dev/zero | tr '\0' a)" >> "$1/.planar-journal"
  done
}
j_other_stamp_name() { printf 'planar-install 1\n' > "$1/.planar-install.tmp"; }
for c in j_wrong_root j_wrong_ver j_no_marker j_bad_phase j_no_phase j_no_root j_dup_key j_malformed \
         j_bad_key j_empty j_bare_staging j_symlink j_dir j_nonprint j_del j_oversize j_other_stamp_name; do
  reject_journal "$c" "$c"
done

# A journal for another root copied into this one is forged evidence.
P="$TMP/jcopy/.planar"; Q="$TMP/jcopy-src/.planar"
write_journal "$Q" "planar-journal 1" "root=$Q" "phase=mutating"
mkdir -p "$P"; cp "$Q/.planar-journal" "$P/.planar-journal"
expect_ownership_refusal "journal copied from another root" "$P" install
# --force still gets past the guard of a root whose journal is not valid, but
# the install state machine never treats a journal it cannot validate as
# permission to proceed: it refuses, keeping the file. With the file moved
# away, --force adopts the root.
before="$(snapshot "$P")"
run_installer "$(dirname "$P")" 0 --prefix "$P" --force
[[ "$RC" == 1 ]] || fail "--force installed over an invalid journal ($RC): $(cat "$TMP/err")"
! grep -Fq 'does not look like a Planar install' "$TMP/err" || fail "--force did not get past the guard"
grep -Fq 'is not a valid recovery journal' "$TMP/err" || fail "the invalid journal was not named: $(cat "$TMP/err")"
[[ "$before" == "$(snapshot "$P")" ]] || fail "the refusal over an invalid journal changed the root"
mv "$P/.planar-journal" "$TMP/jcopy-forged-journal"
run_installer "$(dirname "$P")" 0 --prefix "$P" --force
[[ "$RC" == 0 && -f "$P/.planar-install" ]] || fail "--force did not adopt the root once the forged journal was gone: $(cat "$TMP/err")"
pass

# A journal reached through a symlinked prefix names the canonical root.
mkdir -p "$TMP/jlink-real/.planar"
write_journal "$TMP/jlink-real/.planar" "planar-journal 1" "root=$TMP/jlink-real/.planar" "phase=prepared"
mkdir -p "$TMP/jlink"
ln -s "$TMP/jlink-real/.planar" "$TMP/jlink/.planar"
expect_adopted "valid journal through a symlinked prefix" "$TMP/jlink/.planar"

# A well-formed journal for a root under a non-ASCII home is adopted without --force.
NA="$TMP/jos$(printf '\303\251')/.planar"
write_journal "$NA" "planar-journal 1" "root=$NA" "phase=prepared"
printf 'partial\n' > "$NA/.staging-bin"
expect_adopted "valid journal under a non-ASCII root" "$NA"

# --- planar-uninstall runs the same guard ---------------------------------------

# The standalone uninstaller, run directly (not through install.sh --uninstall):
# $HOME, / and the empty string exit 2 before anything is removed, with the shim
# recording any removal attempt; --force is refused naming --purge.
run_uninstaller() {
  local home="$1" shimmed="$2"; shift 2
  local path="$PATH"
  [[ "$shimmed" == 1 ]] && path="$SHIM:$path"
  : > "$SHIMLOG"
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME PATH="$path" HOME="$home" PLANAR_DB="$home/planar.db" NO_COLOR=1 \
    ${RUN_PLANAR_HOME+PLANAR_HOME="$RUN_PLANAR_HOME"} \
    /bin/bash "$REPO/scripts/uninstall.sh" "$@" </dev/null >"$TMP/out" 2>"$TMP/err" || RC=$?
}
H="$(new_home direct)"
for root in "$H" "/" ""; do
  for force in 0 1; do
    flags=()
    [[ "$force" == 1 ]] && flags+=(--force)
    before="$(snapshot "$H")"
    run_uninstaller "$H" 1 --prefix "$root" ${flags[@]+"${flags[@]}"}
    [[ "$RC" == 2 ]] || fail "planar-uninstall --prefix '$root' (force=$force): expected exit 2, got $RC: $(cat "$TMP/err")"
    if [[ "$force" == 1 ]]; then
      grep -Fq -- "--purge" "$TMP/err" || fail "planar-uninstall --force: the refusal does not name --purge: $(cat "$TMP/err")"
    else
      grep -Fq "Rule:" "$TMP/err" || fail "planar-uninstall --prefix '$root': the rule is not named: $(cat "$TMP/err")"
      [[ -z "$root" ]] || grep -Fq "$root" "$TMP/err" || fail "planar-uninstall --prefix '$root': the path is not named"
    fi
    [[ ! -s "$SHIMLOG" && "$before" == "$(snapshot "$H")" ]] || fail "planar-uninstall --prefix '$root' (force=$force) touched the arena: $(cat "$SHIMLOG")"
    pass
  done
done
before="$(snapshot "$H")"
RUN_PLANAR_HOME=""
run_uninstaller "$H" 1
unset RUN_PLANAR_HOME
[[ "$RC" == 2 ]] && grep -Fq "empty string" "$TMP/err" || fail "planar-uninstall with PLANAR_HOME= was not refused naming the empty string ($RC): $(cat "$TMP/err")"
[[ ! -s "$SHIMLOG" && "$before" == "$(snapshot "$H")" ]] || fail "planar-uninstall with PLANAR_HOME= touched the arena"
pass
FD="$(make_foreign foreign-direct)"
expect_direct_ownership_refusal() {
  local before
  before="$(snapshot "$FD")"
  run_uninstaller "$(dirname "$FD")" 1 --prefix "$FD"
  [[ "$RC" == 1 ]] && grep -Fq "does not look like a Planar install" "$TMP/err" || fail "planar-uninstall over a foreign root: expected exit 1 ($RC): $(cat "$TMP/err")"
  [[ ! -s "$SHIMLOG" && "$before" == "$(snapshot "$FD")" ]] || fail "planar-uninstall over a foreign root touched it"
  pass
}
expect_direct_ownership_refusal

# --- the uninstalled marker ----------------------------------------------------

# What a completed uninstall leaves in a root it keeps: validated evidence that
# names the root (journal.sh). A valid one is adopted without --force; a copy
# from another root, a symlink, a malformed or extended one never is.
P="$TMP/marker-ok/.planar"
mkdir -p "$P/research"
printf 'paper\n' > "$P/research/draft.md"
printf 'planar-uninstalled 1\nroot=%s\n' "$P" > "$P/.planar-uninstalled"
expect_adopted "valid uninstalled marker" "$P"
[[ ! -e "$P/.planar-uninstalled" && "$(cat "$P/research/draft.md")" == paper ]] || fail "the install did not supersede the marker, or touched the kept entry"
bad_marker() {
  local label="$1" P="$TMP/marker-$1/.planar"; shift
  mkdir -p "$P"
  printf 'x\n' > "$P/thing"
  "$@" "$P"
  expect_ownership_refusal "uninstalled marker: $label" "$P" install
  expect_ownership_refusal "uninstalled marker: $label" "$P" uninstall
}
m_other_root() {
  local Q="$TMP/marker-src/.planar"
  mkdir -p "$Q"
  printf 'planar-uninstalled 1\nroot=%s\n' "$Q" > "$Q/.planar-uninstalled"
  cp "$Q/.planar-uninstalled" "$1/.planar-uninstalled"
}
m_symlink() {
  printf 'planar-uninstalled 1\nroot=%s\n' "$1" > "$TMP/marker-link-target"
  ln -s "$TMP/marker-link-target" "$1/.planar-uninstalled"
}
m_extra_line() { printf 'planar-uninstalled 1\nroot=%s\nextra=1\n' "$1" > "$1/.planar-uninstalled"; }
m_wrong_ver()  { printf 'planar-uninstalled 2\nroot=%s\n' "$1" > "$1/.planar-uninstalled"; }
m_empty()      { : > "$1/.planar-uninstalled"; }
m_dir()        { mkdir -p "$1/.planar-uninstalled"; }
m_control()    { printf 'planar-uninstalled 1\nroot=%s\001\n' "$1" > "$1/.planar-uninstalled"; }
m_lookalike()  { printf 'uninstalled\n' > "$1/.planar-uninstalled.bak"; printf 'x\n' > "$1/uninstalled"; }
for c in m_other_root m_symlink m_extra_line m_wrong_ver m_empty m_dir m_control m_lookalike; do
  bad_marker "$c" "$c"
done

# --- $HOME/.planar under another spelling (identity, -ef) ---------------------

# Preserved data paths are evidence only in $HOME/.planar. When $HOME/.planar is
# a symlink, the canonical root is its target, which no string comparison with
# "$HOME/.planar" matches: only the identity check (-ef) adopts it.
HL="$TMP/ef-link/user"
mkdir -p "$HL" "$TMP/ef-link/real-planar/workbench"
printf 'w\n' > "$TMP/ef-link/real-planar/workbench/doc.md"
ln -s "$TMP/ef-link/real-planar" "$HL/.planar"
run_installer "$HL" 0
[[ "$RC" == 0 ]] || fail "a symlinked \$HOME/.planar holding only a data path was refused ($RC): $(cat "$TMP/err")"
[[ -x "$TMP/ef-link/real-planar/bin/planar" && "$(cat "$TMP/ef-link/real-planar/workbench/doc.md")" == w ]] \
  || fail "the install through a symlinked \$HOME/.planar did not install into its target, or touched the data path"
pass
# The same data through a second, unrelated symlink is not $HOME/.planar: refused.
HO="$TMP/ef-other/user"
mkdir -p "$HO" "$TMP/ef-other/real/workbench"
printf 'w\n' > "$TMP/ef-other/real/workbench/doc.md"
ln -s "$TMP/ef-other/real" "$TMP/ef-other/alias"
before="$(snapshot "$TMP/ef-other/real")"
run_installer "$HO" 1 --prefix "$TMP/ef-other/alias"
[[ "$RC" == 1 ]] && grep -Fq "$TMP/ef-other/real does not look like a Planar install" "$TMP/err" \
  || fail "a data path behind a symlink that is not \$HOME/.planar was adopted ($RC): $(cat "$TMP/err")"
[[ ! -s "$SHIMLOG" && "$before" == "$(snapshot "$TMP/ef-other/real")" ]] || fail "the refused alias root changed"
pass
# A case variant of $HOME/.planar names the same directory on a case-insensitive
# volume; the canonical string differs, so only -ef adopts it.
HC="$TMP/ef-case/user"
mkdir -p "$HC/.planar/workbench"
printf 'w\n' > "$HC/.planar/workbench/doc.md"
if [[ -d "$HC/.PLANAR" ]]; then
  run_installer "$HC" 0 --prefix "$HC/.PLANAR"
  [[ "$RC" == 0 ]] || fail "a case-variant spelling of \$HOME/.planar holding only a data path was refused ($RC): $(cat "$TMP/err")"
  [[ -x "$HC/.planar/bin/planar" && "$(cat "$HC/.planar/workbench/doc.md")" == w ]] || fail "the case-variant install did not land in \$HOME/.planar"
  pass
else
  printf 'install-prefix-guard-test: skipping the case-variant $HOME/.planar adoption fixture: the scratch filesystem is case-sensitive\n'
fi

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
