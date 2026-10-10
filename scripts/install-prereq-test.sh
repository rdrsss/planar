#!/usr/bin/env bash
# install-prereq-test.sh -- every program the operator path runs is listed.
#
# The operator path is `planar update` (src/cmd/planar/handlers/update/) and the
# bootstrap scripts/get-planar.sh. Each program either one runs must be named in
# INSTALL.md § Prerequisites, and each program `planar update` runs also in
# install.sh's BASE_DEPS or RUN_DEPS (the installer's preflight warns about it).
# GNU wget, which only scripts/get-planar-test.sh needs, must be on the
# Homebrew line of INSTALL.md § Contributor prerequisites.
#
# How the programs are found (scripts/install-prereq-scan.py does the scanning):
#   - planar update: every spawn API call in the non-test update sources is
#     counted and the counts are PINNED (UPDATE_SITES), so a new spawn site of
#     any kind (process::capture, run_inherited, runner::start, execve, ...)
#     fails here until it is reviewed and the pin moved. Each program a site
#     names (a literal, an env-wrapped argv head, a constexpr path) must be in
#     UPDATE_PROGRAMS; a program held in a variable shows as its own name and
#     fails the same way.
#   - get-planar.sh: the word in every command position is read with a small
#     sh lexer (so `tar`, `mktemp`, `id` count as much as a `command -v` probe).
#     Each must be a builtin or the script's own function, in BOOTSTRAP_PROGRAMS,
#     in install.sh's manifests, or on the named BOOTSTRAP_BASE allowlist.
# Every pin is also checked the other way against the sources so it cannot rot.
# bash 3.2: no associative arrays, no mapfile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail() { printf 'install-prereq-test: %s\n' "$*" >&2; exit 1; }
# Overridable so a probe can run the test against a mutated copy of the files.
INSTALL_SH="${PREREQ_INSTALL_SH:-$ROOT/install.sh}"
INSTALL_MD="${PREREQ_INSTALL_MD:-$ROOT/INSTALL.md}"
UPDATE_DIR="${PREREQ_UPDATE_DIR:-$ROOT/src/cmd/planar/handlers/update}"
BOOTSTRAP="${PREREQ_BOOTSTRAP:-$ROOT/scripts/get-planar.sh}"

# Programs planar update spawns, and the spawn call sites that name them.
UPDATE_PROGRAMS="bash tar ldd env ps ioreg"
UPDATE_SITES="capture=4 execv=1 resolve_program=1"
# Programs the bootstrap runs that a host may lack; INSTALL.md § Prerequisites names each.
BOOTSTRAP_PROGRAMS="curl wget shasum sha256sum tar ldd bash"
# Command-position words that are neither pinned above nor in install.sh's
# manifests, each with the reason it needs no entry. Every one must still be run.
BOOTSTRAP_BASE="sw_vers id tail"
base_reason() {
  case "$1" in
    sw_vers) echo "macOS system tool in /usr/bin on every Mac; the script runs it only on Darwin and refuses when it fails" ;;
    id) echo "POSIX base utility (coreutils, BSD); the script reads the invoking user with it" ;;
    tail) echo "POSIX base utility shipped with head, which BASE_DEPS lists" ;;
  esac
}
# Shell builtins and keywords the sh lexer reports as command words.
SH_BUILTINS=": [ break cd command continue echo exit export false printf pwd read return set shift test trap true unset"

in_words() { case " $2 " in *" $1 "*) return 0 ;; esac; return 1; }

# --- the manifests of install.sh and the Prerequisites sections of INSTALL.md ---
dep_names() { # dep_names ARRAY... -- the command names of the listed arrays' entries, basenames
  sed -n "/^BASE_DEPS=(/,/^if \[\[ \"\$PREBUILT\" -eq 1/p" "$INSTALL_SH" \
    | sed -n 's/^[[:space:]]*\(RUN_DEPS+=(\)\{0,1\}"\([^|"]*\)|.*/\2/p' | sed 's|.*/||'
}
INSTALL_DEPS="$(dep_names | tr '\n' ' ')"
[[ -n "$INSTALL_DEPS" ]] || fail "found no dependency entries in $INSTALL_SH"
PREREQ="$(awk '/^## Prerequisites/{f=1;next} /^## /{f=0} f' "$INSTALL_MD")"
[[ -n "$PREREQ" ]] || fail "$INSTALL_MD has no '## Prerequisites' section"
CONTRIB="$(awk '/^### Contributor prerequisites/{f=1;next} /^##/{f=0} f' "$INSTALL_MD")"

in_prereq() { # in_prereq NAME -- NAME is a code span (or the tail of a path in one) in § Prerequisites
  printf '%s\n' "$PREREQ" | grep -Eq "\`([^\`]*/)?$1\`|\`$1 "
}

# --- 1. what planar update spawns -------------------------------------------------
SCAN="$ROOT/scripts/install-prereq-scan.py"
UPDATE_SCAN="$(python3 "$SCAN" update "$UPDATE_DIR")" || fail "scanning $UPDATE_DIR failed"
SEEN_UPDATE="$(printf '%s\n' "$UPDATE_SCAN" | sed -n 's/^program //p' | tr '\n' ' ')"
SEEN_SITES="$(printf '%s\n' "$UPDATE_SCAN" | sed -n 's/^site \([a-z_]*\) \([0-9]*\)$/\1=\2/p' | tr '\n' ' ')"
[[ -n "$SEEN_SITES" ]] || fail "found no spawn call site in $UPDATE_DIR; has the update handler moved?"
[[ "${SEEN_SITES% }" == "$UPDATE_SITES" ]] \
  || fail "the spawn call sites of $UPDATE_DIR changed: found '${SEEN_SITES% }', pinned '$UPDATE_SITES'. A new or removed process::capture/run_inherited/runner::start/execve/... call may run a program that is not listed; review it, then update UPDATE_SITES, UPDATE_PROGRAMS, install.sh RUN_DEPS and INSTALL.md § Prerequisites"
for p in $SEEN_UPDATE; do
  in_words "$p" "$UPDATE_PROGRAMS" || fail "planar update spawns '$p' ($UPDATE_DIR) but it is not in this test's UPDATE_PROGRAMS; list it there, in install.sh RUN_DEPS and in INSTALL.md § Prerequisites"
done
for p in $UPDATE_PROGRAMS; do
  in_words "$p" "$SEEN_UPDATE" || fail "UPDATE_PROGRAMS pins '$p' but the update sources no longer spawn it; drop it from the pin and the manifests"
  in_words "$p" "$INSTALL_DEPS" || fail "planar update runs '$p' but install.sh's BASE_DEPS/RUN_DEPS do not list it"
  in_prereq "$p" || fail "planar update runs '$p' but INSTALL.md § Prerequisites does not name it"
done

# --- 2. what the bootstrap runs ---------------------------------------------------
SEEN_BOOT="$(python3 "$SCAN" bootstrap "$BOOTSTRAP" | sed -n 's/^cmd //p' | tr '\n' ' ')" || fail "scanning $BOOTSTRAP failed"
[[ -n "$SEEN_BOOT" ]] || fail "found no command in $BOOTSTRAP"
OWN_FUNCS="$(grep -oE '^[A-Za-z_][A-Za-z0-9_]*\(\)' "$BOOTSTRAP" | tr -d '()' | tr '\n' ' ')"
RUN_BOOT=""
for p in $SEEN_BOOT; do
  in_words "$p" "$SH_BUILTINS" && continue
  in_words "$p" "$OWN_FUNCS" && continue
  [[ "$p" == planar ]] && continue   # the product itself, probed to see whether it is on PATH
  RUN_BOOT="$RUN_BOOT $p"
  in_words "$p" "$BOOTSTRAP_PROGRAMS" && continue
  in_words "$p" "$INSTALL_DEPS" && continue
  in_words "$p" "$BOOTSTRAP_BASE" && continue
  fail "$BOOTSTRAP runs '$p' but it is not in this test's BOOTSTRAP_PROGRAMS, install.sh's BASE_DEPS/RUN_DEPS or the BOOTSTRAP_BASE allowlist; list it in INSTALL.md § Prerequisites and one of them"
done
for p in $BOOTSTRAP_BASE; do
  in_words "$p" "$RUN_BOOT" || fail "BOOTSTRAP_BASE allows '$p' ($(base_reason "$p")) but $BOOTSTRAP no longer runs it; drop it"
done
for p in $BOOTSTRAP_PROGRAMS; do
  in_words "$p" "$RUN_BOOT" || fail "BOOTSTRAP_PROGRAMS pins '$p' but $BOOTSTRAP no longer runs it"
  in_prereq "$p" || fail "the bootstrap runs '$p' but INSTALL.md § Prerequisites does not name it"
done

# --- 3. the test-only prerequisite --------------------------------------------------
printf '%s\n' "$CONTRIB" | grep -E '^brew install ' | grep -Eq '(^| )wget( |$)' \
  || fail "wget is not on the Homebrew line of INSTALL.md § Contributor prerequisites (scripts/get-planar-test.sh needs GNU wget)"
printf '%s\n' "$CONTRIB" | grep -Eqi 'wget.*test-only|test-only.*wget' \
  || fail "INSTALL.md § Contributor prerequisites does not say wget is test-only"
grep -Fq 'GNU wget is a declared dependency of this test' "$ROOT/scripts/get-planar-test.sh" \
  || fail "scripts/get-planar-test.sh no longer declares its wget dependency; revisit the wget prerequisite"

printf 'install-prereq-test: every program planar update (%s) and the bootstrap (%s) run is listed\n' "$UPDATE_PROGRAMS" "$BOOTSTRAP_PROGRAMS"
