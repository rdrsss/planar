#!/usr/bin/env bash
# The standalone uninstaller (plan 1122, task rel-uninstall-script; tech spec
# 677, "The uninstaller" and "Mutation ownership and cleanup"; test spec 679,
# the scenarios that verify rel-uninstall-script).
#
# Every run is a prebuilt install of a fake bundle (scripts/fixtures/
# prebuilt-bundle.sh: five tiny stub executables, labelled as such) into a
# scratch HOME, then an uninstall, under /bin/bash with a PATH of install.sh's
# base tier only: no python3, no flock. Nothing outside the scratch arena is
# read or written; every run sets HOME and PLANAR_DB inside it.
# shellcheck disable=SC2016,SC2012,SC2015,SC1091  # literal $ in fixtures; ls for messages; A && B || fail is intended
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
BG=()
cleanup() {
  local p
  for p in ${BG[@]+"${BG[@]}"}; do kill -9 "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  chmod -R u+rwx "$TMP" 2>/dev/null || true
  [[ -n "${KEEP:-}" ]] || rm -rf "$TMP"
}
trap cleanup EXIT
fail() { printf 'install-uninstall-test: FAIL: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); printf 'ok %s %s\n' "$PASSED" "$1"; }
# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"

# --- the PATH: install.sh's base tier only ----------------------------------------------

BASEBIN="$TMP/basebin"
mkdir -p "$BASEBIN"
base_names="$(sed -n '/^BASE_DEPS=(/,/^)/p' "$ROOT/install.sh" | sed -n 's/^  "\([^|"]*\)|.*/\1/p')"
[[ -n "$base_names" ]] || fail "could not read BASE_DEPS from install.sh"
for n in $base_names; do
  found="$(command -v "$n" || true)"
  [[ -n "$found" ]] || fail "base tool $n is not on this host"
  ln -s "$found" "$BASEBIN/$n"
done
for n in python3 python flock; do [[ ! -e "$BASEBIN/$n" ]] || fail "$n is in the base tier"; done
ln -s /bin/bash "$BASEBIN/bash"

# --- the fake bundle ----------------------------------------------------------------------

BUNDLE="$TMP/bundles/planar-fake"
fake_bundle_make "$ROOT" "$BUNDLE"

# --- arena helpers --------------------------------------------------------------------------

FAULTDIR="$TMP/fault"
mkdir -p "$FAULTDIR"
# new_home NAME -- a scratch HOME with Claude and Codex present.
new_home() {
  local h="$TMP/homes/$1"
  mkdir -p "$h/.claude" "$h/.codex" "$h/work"
  printf '%s' "$h"
}
# in_arena HOME [ENV=V...] -- CMD...: run CMD from HOME/work with a scrubbed
# environment rooted at HOME. RC, $TMP/out, $TMP/err. Stdin is closed.
in_arena() {
  local home="$1"; shift
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  RC=0
  ( cd "$home/work" && /usr/bin/env -i HOME="$home" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
      PLANAR_DB="$home/.planar/planar.db" \
      PLANAR_INSTALL_TEST_FAULT_ARMED=test-only PLANAR_INSTALL_TEST_FAULT_DIR="$FAULTDIR" \
      ${envs[@]+"${envs[@]}"} /bin/bash "$@" </dev/null >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
# install HOME [ENV=V...] -- [ARGS...]: a prebuilt install of the fake bundle.
install() {
  local home="$1"; shift
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  in_arena "$home" ${envs[@]+"${envs[@]}"} -- "$BUNDLE/install.sh" --prebuilt "$BUNDLE" "$@"
}
# install_ok HOME [ENV=V...] -- [ARGS...]: install and require success.
install_ok() {
  install "$@"
  [[ "$RC" == 0 ]] || fail "install into $1 failed ($RC): $(show)"
}
# legacy_uninstall HOME [ENV=V...] -- [ARGS...]: `install.sh --uninstall`.
legacy_uninstall() {
  local home="$1"; shift
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  in_arena "$home" ${envs[@]+"${envs[@]}"} -- "$BUNDLE/install.sh" --uninstall "$@"
}
show() { printf -- '--- stdout\n%s\n--- stderr\n%s\n' "$(cat "$TMP/out")" "$(cat "$TMP/err")"; }
# sums DIR -- every path and file checksum under DIR (lock records left out).
sums() { ( cd "$1" && find . -print | LC_ALL=C sort && find . -type f -exec cksum {} + | LC_ALL=C sort ); }

# --- unknown entries beside preserved data are kept and reported ------------------------------

H="$(new_home unknown)"; P="$H/.planar"
install_ok "$H"
printf 'mine\n' > "$P/notes.txt"
mkdir -p "$P/bin.old" "$P/.staging-foreign" "$P/research"
printf 'old binary I kept\n' > "$P/bin.old/planar"
printf 'half\n' > "$P/.staging-foreign/x"
printf 'paper\n' > "$P/research/draft.md"
before="$(sums "$P/bin.old")$(sums "$P/.staging-foreign")$(sums "$P/research")$(cksum < "$P/notes.txt")"
legacy_uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall with unknown entries failed ($RC): $(show)"
[[ ! -e "$P/bin" && ! -e "$P/skills" && ! -e "$P/install-manifest.json" ]] || fail "uninstall left managed trees: $(ls -A "$P" | tr '\n' ' ')"
for e in notes.txt bin.old .staging-foreign research; do
  [[ -e "$P/$e" ]] || fail "uninstall removed the unknown entry $P/$e: $(show)"
  grep -Fq "$P/$e" "$TMP/out" "$TMP/err" || fail "uninstall did not report the unknown entry $P/$e it kept: $(show)"
done
[[ "$before" == "$(sums "$P/bin.old")$(sums "$P/.staging-foreign")$(sums "$P/research")$(cksum < "$P/notes.txt")" ]] \
  || fail "uninstall changed the bytes of an unknown entry"
pass "uninstall keeps and reports unknown entries, unknown .old and .staging-* included"

# --- a non-default root left holding only preserved data is adopted again ------------------

H="$(new_home alt-root)"; ALT="$H/alt/inst"; EXT="$H/ext"
mkdir -p "$EXT" "$H/alt"
install_ok "$H" "PLANAR_DB=$EXT/p.db" -- --prefix "$ALT"
printf 'edited\n' > "$ALT/templates/a.toml"
legacy_uninstall "$H" "PLANAR_DB=$EXT/p.db" -- --prefix "$ALT"
[[ "$RC" == 0 ]] || fail "uninstall of a non-default root failed ($RC): $(show)"
[[ -d "$ALT" && ! -e "$ALT/bin" && ! -e "$ALT/.planar-install" && ! -e "$ALT/planar.db" ]] \
  || fail "the fixture is not the intended shape after uninstall: $(ls -A "$ALT" 2>/dev/null | tr '\n' ' ')"
tpl_before="$(cksum < "$ALT/templates/a.toml")"
install "$H" "PLANAR_DB=$EXT/p.db" -- --prefix "$ALT"
[[ "$RC" == 0 ]] || fail "a reinstall at a non-default root after uninstall was refused without --force ($RC): $(show)"
[[ -x "$ALT/bin/planar" && -f "$ALT/.planar-install" ]] || fail "the reinstall at the non-default root did not install"
[[ "$tpl_before" == "$(cksum < "$ALT/templates/a.toml")" ]] || fail "the reinstall changed a preserved template"
pass "a non-default root left holding only preserved data is reinstalled without --force"

printf 'install uninstall tests: %s passed\n' "$PASSED"
