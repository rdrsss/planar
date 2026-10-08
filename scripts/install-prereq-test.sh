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
# How the programs are found. The two lists below are PINNED, because a shell
# script and C++ argv arrays have no robust common parser. Each pin is checked
# both ways against the sources so neither can rot:
#   - every program the sources are seen to spawn (process::capture("x"),
#     resolve_program(env, "x"), an `.argv = {"x"` head, an env-wrapped
#     `{"LC_ALL=C", "x"` head; `command -v x` in get-planar.sh) must be in the
#     pin, so a newly added program fails here until it is listed everywhere;
#   - every pinned program must still appear in its sources.
# bash 3.2: no associative arrays, no mapfile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail() { printf 'install-prereq-test: %s\n' "$*" >&2; exit 1; }
# Overridable so a probe can run the test against a mutated copy of the files.
INSTALL_SH="${PREREQ_INSTALL_SH:-$ROOT/install.sh}"
INSTALL_MD="${PREREQ_INSTALL_MD:-$ROOT/INSTALL.md}"
UPDATE_DIR="${PREREQ_UPDATE_DIR:-$ROOT/src/cmd/planar/handlers/update}"
BOOTSTRAP="${PREREQ_BOOTSTRAP:-$ROOT/scripts/get-planar.sh}"

# Programs planar update spawns, and programs the bootstrap runs that a host may lack.
UPDATE_PROGRAMS="bash tar ldd env ps ioreg"
BOOTSTRAP_PROGRAMS="curl wget shasum sha256sum tar ldd bash"

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
SEEN_UPDATE="$(python3 - "$UPDATE_DIR" <<'PY'
import glob, re, sys
found = set()
for path in glob.glob(sys.argv[1] + "/*.cpp"):
    if path.endswith(".t.cpp"):
        continue
    src = open(path, encoding="utf-8").read()
    for rx in (r'process::capture\(\s*"([^"]+)"',
               r'resolve_program\([^,()]*(?:\([^()]*\))?[^,()]*,\s*"([^"]+)"',
               r'\.argv\s*=\s*\{\s*"([^"]+)"',
               r'\{\s*(?:"[A-Za-z_]+=[^"]*",\s*)+"([^"]+)"'):
        for m in re.finditer(rx, src):
            found.add(m.group(1).rsplit("/", 1)[-1])
print(" ".join(sorted(found)))
PY
)"
for p in $SEEN_UPDATE; do
  in_words "$p" "$UPDATE_PROGRAMS" || fail "planar update spawns '$p' ($UPDATE_DIR) but it is not in this test's UPDATE_PROGRAMS; list it there, in install.sh RUN_DEPS and in INSTALL.md § Prerequisites"
done
for p in $UPDATE_PROGRAMS; do
  in_words "$p" "$SEEN_UPDATE" || grep -Eq "\"([^\"]*/)?$p\"" "$UPDATE_DIR"/*.cpp || fail "UPDATE_PROGRAMS pins '$p' but the update sources no longer spawn it; drop it from the pin and the manifests"
  in_words "$p" "$INSTALL_DEPS" || fail "planar update runs '$p' but install.sh's BASE_DEPS/RUN_DEPS do not list it"
  in_prereq "$p" || fail "planar update runs '$p' but INSTALL.md § Prerequisites does not name it"
done

# --- 2. what the bootstrap runs ---------------------------------------------------
SEEN_BOOT="$(grep -o 'command -v [a-z0-9_]*' "$BOOTSTRAP" | sed 's/command -v //' | grep -vx planar | sort -u | tr '\n' ' ')"
for p in $SEEN_BOOT; do
  in_words "$p" "$BOOTSTRAP_PROGRAMS" || fail "$BOOTSTRAP probes '$p' but it is not in this test's BOOTSTRAP_PROGRAMS; list it there and in INSTALL.md § Prerequisites"
done
for p in $BOOTSTRAP_PROGRAMS; do
  grep -Eq "(^|[^a-z0-9_-])$p([^a-z0-9_-]|\$)" "$BOOTSTRAP" || fail "BOOTSTRAP_PROGRAMS pins '$p' but $BOOTSTRAP no longer mentions it"
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
