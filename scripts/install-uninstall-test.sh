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
# shellcheck disable=SC2016,SC2012,SC2015,SC1091,SC2086,SC2010,SC2088  # literal $ and ~ in fixtures; ls for messages; A && B || fail is intended; optional args unquoted
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
pass() { PASSED=$((PASSED + 1)); printf 'ok %s %s (%ss)\n' "$PASSED" "$1" "$SECONDS"; SECONDS=0; }
# Test groups (plan 1122 M5, task 7361). The whole file took 428 to 510 s on a loaded host,
# so ctest registers one entry per group (install.uninstall_<group>, label
# install_uninstall_<group>). INSTALL_UNINSTALL_GROUP selects one; unset runs all three in
# file order, as before. An unknown name is a usage error.
UNINSTALL_GROUPS="removal manifest interrupted"
GROUP="${INSTALL_UNINSTALL_GROUP:-all}"
case " all $UNINSTALL_GROUPS " in *" $GROUP "*) ;; *) printf 'install-uninstall-test: unknown INSTALL_UNINSTALL_GROUP %s (want one of: %s)\n' "$GROUP" "$UNINSTALL_GROUPS" >&2; exit 2 ;; esac
# Scenario selection and parallel dispatch (plan 1122 M6, task 7434; scripts/fixtures/scenario-runner.sh).
# INSTALL_TEST_SCENARIO=<name> runs only that scenario, inside or outside its group; an unknown
# name exits 2. A run that selects several scenarios runs INSTALL_TEST_JOBS of them at a time
# (default 4), each as a child of this script with its own scratch directory, homes and HOME-
# derived roots, locks and databases. The three loops of the --purge config scenario are
# separate names (purgecfg-keys, purgecfg-spellings, purgecfg-outside). The table is the dispatch
# order, name:group. The serial scenarios hold a mutation lock from a second process or pause an
# uninstaller while an install is refused, and run alone after the parallel batch (decision 1328:
# the lock serializes mutation of one root, and these scenarios test that serialization).
SCEN_TABLE="purgecfg-spellings:manifest purgecfg-keys:manifest purgecfg-outside:manifest verify:interrupted source-retry:interrupted happy:removal killed:interrupted upgrade-terminate:interrupted upgrade-purge:interrupted nomanifest:manifest link:removal unowned:removal purge:removal escaped:manifest truncated:manifest localbin:manifest reloc:manifest unknown:removal alt-root:removal force:removal reinstall:removal killed-vendor:interrupted held:manifest paused:manifest"
SCEN_SERIAL=" held paused "
# shellcheck source=fixtures/scenario-runner.sh
source "$ROOT/scripts/fixtures/scenario-runner.sh"
scen_init install-uninstall-test "$SCEN_TABLE" "$SCEN_SERIAL"
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

if [[ -n "${INSTALL_TEST_SHARED-}" ]]; then
  # A parallel child: the dispatching run staged both bundles; they are read-only here.
  BUNDLE="$INSTALL_TEST_SHARED/bundles/planar-fake"
  B2="$INSTALL_TEST_SHARED/bundles/planar-fake-2"
else
  BUNDLE="$TMP/bundles/planar-fake"
  fake_bundle_make "$ROOT" "$BUNDLE"
  # A second bundle, release v1.2.4, whose binaries differ.
  B2="$TMP/bundles/planar-fake-2"
  fake_bundle_make "$ROOT" "$B2"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do stub_binary_write "$B2/bin/$b" v1.2.4 fedcba987654; done
  sed -i.bak 's/"version": "v1.2.3"/"version": "v1.2.4"/' "$B2/release.json" && rm -f "$B2/release.json.bak"
fi
scen_dispatch "$TMP/bundles" "$@"

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
# sums DIR -- every path and file checksum under DIR.
sums() { ( cd "$1" && find . -print | LC_ALL=C sort && find . -type f -exec cksum {} + | LC_ALL=C sort ); }
# uninstall HOME [ENV=V...] -- [ARGS...]: the installed ~/.planar/bin/planar-uninstall.
uninstall() {
  local home="$1"; shift
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  in_arena "$home" ${envs[@]+"${envs[@]}"} -- "$home/.planar/bin/planar-uninstall" "$@"
}
# bundle_uninstall HOME BUNDLE [ENV=V...] -- [ARGS...]: a bundle's uninstall.sh.
bundle_uninstall() {
  local home="$1" bundle="$2"; shift 2
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  in_arena "$home" ${envs[@]+"${envs[@]}"} -- "$bundle/uninstall.sh" "$@"
}
# tree_state HOME -- the HOME tree for comparing two arenas: relative paths with
# checksums, the HOME path normalised inside files, the lock records left out.
tree_state() { ( cd "$1" && find . -path ./.planar.lock -prune -o \( -type f -o -type l -o -type d \) -print | LC_ALL=C sort | while IFS= read -r f; do
  if [[ -L "$f" ]]; then printf '%s -> link\n' "$f"
  elif [[ -d "$f" ]]; then printf '%s/\n' "$f"
  else printf '%s %s\n' "$f" "$(LC_ALL=C sed "s#$1#@HOME@#g" "$f" | cksum)"; fi
done ); }
# recorded HOME -- the installed_path of every projection the manifest records,
# read by the test with sed (test tooling; the escapes are not decoded here, so
# fixtures that use this hold plain paths).
recorded() { sed -n 's/.*"installed_path": "\([^"]*\)".*/\1/p' "$1/.planar/install-manifest.json"; }
# lock_released ROOT -- the highest generation in ROOT.lock is released, and a
# fresh owner can take the lock (it is free); ROOT.lock still exists.
lock_released() {
  local l="$1.lock" g=0 f n
  [[ -d "$l" && ! -L "$l" ]] || fail "the mutation lock directory $l is gone"
  for f in "$l"/owner.*; do [[ -e "$f" ]] || continue; n="${f##*.}"; [[ "$n" -gt "$g" ]] && g="$n"; done
  [[ "$g" -gt 0 && -e "$l/released.$g" ]] || fail "the uninstall left generation $g of $l held: $(ls "$l")"
  /bin/bash -c 'source "$1"; planar_lock_acquire "$2" install && planar_lock_release' lock "$ROOT/scripts/install-lib/mutation-lock.sh" "$1" \
    || fail "the mutation lock of $1 is not free after the uninstall"
}
# hold_lock ROOT OP -- a background process that owns ROOT's mutation lock until
# killed; sets LOCK_HOLDER.
hold_lock() {
  rm -f "$TMP/lock.ready"
  /bin/bash -c 'source "$1"; planar_lock_acquire "$2" "$3" || { echo "$PLANAR_LOCK_ERROR" >&2; exit 1; }; : > "$4"; exec sleep 300' \
    hold "$ROOT/scripts/install-lib/mutation-lock.sh" "$1" "$2" "$TMP/lock.ready" &
  LOCK_HOLDER=$!
  BG+=("$LOCK_HOLDER")
  local i=0
  while [[ ! -e "$TMP/lock.ready" ]]; do i=$((i + 1)); [[ "$i" -lt 600 ]] || fail "the lock holder never acquired"; sleep 0.05; done
}
# seed_data P -- a file in every data path, an edited and a formerly shipped
# (renamed) template, and retired legacy databases, sidecars and logs. Writes
# the list of seeded files to $TMP/seeded.<basename of HOME>.
seed_data() {
  local p="$1"
  mkdir -p "$p/queue-logs" "$p/retired/2026-01-01/queue-logs" "$p/workbench/x" "$p/local/skills/mine" "$p/workspaces" \
           "$p/models" "$p/execute/default" "$p/templates/doc-prompts"
  printf 'SQLite format 3\nseeded\n' > "$p/planar.db"
  printf 'wal\n' > "$p/planar.db-wal"
  printf 'shm\n' > "$p/planar.db-shm"
  printf 'log 1000007\n' > "$p/queue-logs/1000007.log"
  printf 'legacy agent db\n' > "$p/retired/2026-01-01/agent.db"
  printf 'legacy wal\n' > "$p/retired/2026-01-01/agent.db-wal"
  printf 'old log\n' > "$p/retired/2026-01-01/queue-logs/7.log"
  printf 'readme\n' > "$p/workbench/x/README.md"
  printf '[cfg]\n' > "$p/config.toml"
  printf 'skill\n' > "$p/local/skills/mine/SKILL.md"
  printf '{}\n' > "$p/workspaces/w.json"
  printf '{}\n' > "$p/models/catalog.json"
  printf 'p\n' > "$p/execute/default/profile.toml"
  printf 'my edited template\n' > "$p/templates/a.toml"
  printf 'formerly shipped, renamed since\n' > "$p/templates/doc-prompts/old-name.md"
}
data_sums() { ( cd "$1" && find planar.db planar.db-wal planar.db-shm queue-logs retired workbench config.toml local workspaces models execute templates \
  -type f -exec cksum {} + | LC_ALL=C sort ); }
DATA_NAMES="planar.db planar.db-wal planar.db-shm queue-logs retired workbench config.toml local workspaces models execute templates"

# Hoisted out of the first group: every group builds installed homes with it.
make_installed() { # make_installed NAME -- an installed, data-seeded home; prints it
  local h; h="$(new_home "$1")"
  install_ok "$h"
  seed_data "$h/.planar"
  printf 'cleanup list an older install left\n' > "$h/.planar/install-cleanup.txt"
  printf '%s' "$h"
}

if scen unknown; then
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
fi

if scen alt-root; then
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
fi

if scen happy; then
# --- the installed uninstaller removes the install and names what it keeps -----------------

# A prebuilt install with Claude and Codex present, no python3 on PATH. Every path
# the manifest records under the vendor directories, every managed subtree,
# install-cleanup.txt, release.json, the stamp and the manifest go, each named;
# every data path stays byte for byte and is named; the lock is released and
# kept. install.sh --uninstall on an identical arena gives the same end state.
H="$(make_installed happy)"; P="$H/.planar"
cmp -s "$ROOT/scripts/uninstall.sh" "$P/bin/planar-uninstall" && [[ -x "$P/bin/planar-uninstall" ]] \
  || fail "the install did not place scripts/uninstall.sh as an executable bin/planar-uninstall"
recorded "$H" > "$TMP/recorded.happy"
[[ "$(grep -c "$H/.claude/" "$TMP/recorded.happy")" -gt 1 && "$(grep -c "$H/.codex/" "$TMP/recorded.happy")" -gt 1 ]] \
  || fail "the fixture recorded no Claude or Codex projections: $(cat "$TMP/recorded.happy")"
dbefore="$(data_sums "$P")"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "planar-uninstall failed ($RC): $(show)"
while IFS= read -r t; do
  [[ ! -e "$t" && ! -L "$t" ]] || fail "planar-uninstall left the recorded target $t"
  grep -Fq "removed recorded " "$TMP/out" && grep -Fq " $t" "$TMP/out" || fail "planar-uninstall did not name the removal of $t"
done < "$TMP/recorded.happy"
[[ -d "$H/.claude/agents" && -d "$H/.codex/agents" ]] || fail "planar-uninstall removed a vendor directory"
for n in bin skills agents codex-agents workflows scripts migrations; do
  [[ ! -e "$P/$n" ]] || fail "planar-uninstall left the managed subtree $n/"
  grep -Fq "removed managed subtree $P/$n" "$TMP/out" || fail "planar-uninstall did not name the removal of $n/"
done
for f in install-cleanup.txt release.json .planar-install install-manifest.json; do
  [[ ! -e "$P/$f" ]] || fail "planar-uninstall left $f"
  grep -Fq "removed install record $P/$f" "$TMP/out" || fail "planar-uninstall did not name the removal of $f"
done
[[ "$(data_sums "$P")" == "$dbefore" ]] || fail "planar-uninstall changed a data path"
for n in $DATA_NAMES; do
  grep -Fq "kept data path $P/$n" "$TMP/out" || fail "planar-uninstall did not name the kept data path $n: $(show)"
done
[[ "$(ls -A "$P" | LC_ALL=C sort | tr '\n' ' ')" == ".planar-uninstalled $(printf '%s\n' $DATA_NAMES | LC_ALL=C sort | tr '\n' ' ')" ]] \
  || fail "planar-uninstall left more or less than the data paths and its marker: $(ls -A "$P" | tr '\n' ' ')"
[[ ! -e "$P/.planar-journal" ]] || fail "planar-uninstall left its journal"
lock_released "$P"
grep -Fq "if this uninstall is interrupted, finish it with: " "$TMP/out" || fail "planar-uninstall did not print its durable retry"
# install.sh --uninstall is a call into it: an identical arena ends identically.
H2="$(make_installed happy-legacy)"
legacy_uninstall "$H2"
[[ "$RC" == 0 ]] || fail "install.sh --uninstall failed ($RC): $(show)"
diff <(tree_state "$H" | sed "s#/happy/#/X/#g") <(tree_state "$H2" | sed "s#/happy-legacy/#/X/#g") > "$TMP/diff.out" \
  || fail "install.sh --uninstall and planar-uninstall end differently: $(cat "$TMP/diff.out")"
pass "planar-uninstall removes every recorded vendor path, managed subtree and install record, keeps and names every data path; install.sh --uninstall ends the same"
fi

if scen force; then
# --force is refused, by both, at exit 2 naming --purge, before anything changes.
H="$(make_installed force)"; P="$H/.planar"
before="$(tree_state "$H")"
for how in planar-uninstall install.sh; do
  if [[ "$how" == install.sh ]]; then legacy_uninstall "$H" -- --force; else uninstall "$H" -- --force; fi
  [[ "$RC" == 2 ]] || fail "$how --force was not refused at exit 2 ($RC): $(show)"
  grep -Fq -- "--purge" "$TMP/err" || fail "$how --force: the refusal does not name --purge: $(show)"
  [[ "$(tree_state "$H")" == "$before" ]] || fail "$how --force changed the arena"
done
uninstall "$H" -- --bogus
[[ "$RC" == 64 ]] || fail "an unknown option was not a usage error ($RC)"
uninstall "$H" -- --help
[[ "$RC" == 0 ]] && grep -Fq -- "--purge" "$TMP/out" || fail "--help did not print the usage ($RC)"
[[ "$(tree_state "$H")" == "$before" ]] || fail "a usage error or --help changed the arena"
pass "--force is refused at exit 2 naming --purge, by planar-uninstall and install.sh --uninstall, with nothing changed"
fi

if scen reinstall; then
# --- a reinstall after uninstall is accepted --------------------------------------------------

H="$(make_installed reinstall)"; P="$H/.planar"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall before the reinstall failed ($RC): $(show)"
dbefore="$(data_sums "$P")"
install "$H"
[[ "$RC" == 0 ]] || fail "the reinstall after uninstall was refused without --force ($RC): $(show)"
[[ -x "$P/bin/planar" && -f "$P/.planar-install" && ! -e "$P/.planar-uninstalled" ]] || fail "the reinstall did not complete, or left the uninstalled marker"
[[ "$(data_sums "$P")" == "$dbefore" ]] || fail "the reinstall changed a data path"
pass "a reinstall after uninstall completes without --force and keeps the data paths"
fi

if scen purge; then
# --- purge removes the data paths and the empty home -------------------------------------------

H="$(make_installed purge)"; P="$H/.planar"
find "$P/retired" -type f | LC_ALL=C sort > "$TMP/retired.files"
uninstall "$H" -- --purge
[[ "$RC" == 0 ]] || fail "planar-uninstall --purge failed ($RC): $(show)"
[[ ! -e "$P" ]] || fail "--purge left the install root: $(ls -A "$P" | tr '\n' ' ')"
for n in $DATA_NAMES; do
  grep -Fq "purging data path $P/$n" "$TMP/out" || fail "--purge did not name the data path $n before removing it: $(show)"
done
while IFS= read -r f; do
  grep -Fq "purging retired file $f" "$TMP/out" || fail "--purge did not name the retired legacy file $f"
done < "$TMP/retired.files"
lock_released "$P"
pass "--purge names and removes every data path, the retired legacy databases and logs by name, then the empty root; the lock stays and is free"
fi

if scen unowned; then
# --- uninstall skips a vendor file it does not own -----------------------------------------------

H="$(make_installed unowned)"; P="$H/.planar"
recorded "$H" > "$TMP/recorded.unowned"
printf 'my own coder agent\n' > "$H/.claude/agents/planar-coder.md"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall with an unowned vendor file failed ($RC): $(show)"
[[ "$(cat "$H/.claude/agents/planar-coder.md")" == "my own coder agent" ]] || fail "uninstall removed a vendor file the operator replaced"
grep -Fq "left $H/.claude/agents/planar-coder.md" "$TMP/err" || fail "the unowned vendor file was not reported: $(show)"
while IFS= read -r t; do
  [[ "$t" == "$H/.claude/agents/planar-coder.md" ]] && continue
  [[ ! -e "$t" && ! -L "$t" ]] || fail "uninstall left the recorded target $t"
done < "$TMP/recorded.unowned"
pass "a vendor file the operator replaced is left and reported; every other recorded path is removed"
fi

if scen link; then
# A link-mode source install: the managed subtrees are symlinks into the checkout.
# The uninstall unlinks them and the checkout behind them is untouched.
REPO="$TMP/repo"
mkdir -p "$REPO/skills" "$REPO/templates" "$REPO/workflows" "$REPO/migrations"
cp "$ROOT/install.sh" "$REPO/install.sh"
cp -R "$ROOT/scripts" "$REPO/scripts"
rm -rf "$REPO/scripts/__pycache__" "$REPO/scripts/install-lib/__pycache__"
cp -R "$ROOT/agents" "$REPO/agents"
cp -R "$ROOT/skills/planar" "$REPO/skills/planar"
cp "$ROOT/install-cleanup.txt" "$REPO/install-cleanup.txt"
printf 'shipped a\n' > "$REPO/templates/a.toml"
printf -- '-- wf\n' > "$REPO/workflows/w.lua"
printf -- '-- m\n' > "$REPO/migrations/00001_x.up.sql"
: > "$REPO/CMakeLists.txt"
: > "$REPO/CMakePresets.json"
perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$REPO/install.sh"
STUBS="$TMP/stubs"
mkdir -p "$STUBS"
cat > "$STUBS/cmake" <<STUB
#!/usr/bin/env bash
if [[ "\$1" == "--install" ]]; then
  source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
  mkdir -p "\$4/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    stub_binary_write "\$4/bin/\$b" dev
  done
fi
exit 0
STUB
chmod +x "$STUBS/cmake"
H="$(new_home link)"; P="$H/.planar"
repo_before="$(sums "$REPO")"
RC=0
( cd "$H/work" && env -u CODEX_HOME -u PLANAR_HOME PATH="$STUBS:$PATH" HOME="$H" PLANAR_DB="$P/planar.db" NO_COLOR=1 \
    "$REPO/install.sh" --build-dir "$H/build" --link >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 0 ]] || fail "the link-mode source install failed ($RC): $(show)"
rm -rf "$H/build"
for n in scripts workflows migrations; do [[ -L "$P/$n" ]] || fail "the link-mode fixture has no symlinked $n/"; done
[[ -L "$H/.claude/agents/planar-coder.md" ]] || fail "the link-mode fixture placed no symlinked vendor agent"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall of a link-mode install failed ($RC): $(show)"
for n in scripts workflows migrations skills agents; do [[ ! -e "$P/$n" && ! -L "$P/$n" ]] || fail "uninstall left the link-mode $n"; done
[[ ! -e "$H/.claude/agents/planar-coder.md" && ! -L "$H/.claude/agents/planar-coder.md" ]] || fail "uninstall left a link-mode vendor agent"
[[ "$(sums "$REPO")" == "$repo_before" ]] || fail "the uninstall changed the checkout behind the link-mode install"
pass "a link-mode install's symlinked subtrees and vendor links are unlinked; the checkout is untouched"
fi

if scen escaped; then
# --- uninstall reads an escaped path and reports a bad line ------------------------------------

# CODEX_HOME holds a space, a backslash and a double quote; the manifest records
# the Codex agents there with \\ and \" escapes, and the reader undoes them.
H="$(new_home escaped)"; P="$H/.planar"
CX="$H/co dex\\q\"x"
mkdir -p "$CX"
install_ok "$H" "CODEX_HOME=$CX"
[[ -f "$CX/agents/planar-coder.toml" ]] || fail "the fixture placed no Codex agent under the escaped CODEX_HOME"
grep -Fq 'co dex\\q\"x/agents/planar-coder.toml' "$P/install-manifest.json" || fail "the manifest does not hold the escaped path: $(grep -m1 planar-coder.toml "$P/install-manifest.json")"
cx_n="$(ls "$CX/agents" | grep -c '^planar-.*\.toml$')"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall of the escaped-path install failed ($RC): $(show)"
[[ -z "$(ls -A "$CX/agents")" ]] || fail "uninstall left Codex agents under the escaped CODEX_HOME: $(ls -A "$CX/agents")"
[[ "$(grep -Fc "removed recorded codex agent $CX/agents/" "$TMP/out")" == "$cx_n" ]] || fail "uninstall did not name each escaped-path removal: $(show)"
pass "a manifest path with a space, a backslash and a double quote is read and its target removed"
fi

if scen truncated; then
# One projection line truncated mid-object: reported by number, left in place;
# the others are removed.
H="$(new_home truncated)"; P="$H/.planar"
install_ok "$H"
victim="$H/.claude/agents/planar-coder.md"
ln_no="$(grep -n "\"installed_path\": \"$victim\"" "$P/install-manifest.json" | cut -d: -f1)"
[[ -n "$ln_no" ]] || fail "the fixture manifest does not record $victim"
awk -v n="$ln_no" 'NR == n { print substr($0, 1, 60); next } { print }' "$P/install-manifest.json" > "$TMP/m.json"
mv "$TMP/m.json" "$P/install-manifest.json"
recorded "$H" > "$TMP/recorded.truncated"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall over a damaged manifest line failed ($RC): $(show)"
grep -Fq "install-manifest.json line $ln_no: cannot read this projection; its target is left in place" "$TMP/err" \
  || fail "the damaged line $ln_no was not reported by number: $(show)"
[[ -f "$victim" ]] || fail "the target of the damaged line was removed"
while IFS= read -r t; do
  [[ ! -e "$t" && ! -L "$t" ]] || fail "uninstall left $t, recorded on an intact line"
done < "$TMP/recorded.truncated"
pass "a projection line truncated mid-object is reported by number and its target kept; the other targets go"
fi

if scen localbin; then
# --- uninstall offers to remove a binary in the local bin ----------------------------------------

H="$(make_installed localbin)"; P="$H/.planar"
mkdir -p "$H/.local/bin"
printf '#!/bin/sh\n' > "$H/.local/bin/planar"; printf '#!/bin/sh\n' > "$H/.local/bin/planar-agent"
printf 'not planar\n' > "$H/.local/bin/other-tool"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall with ~/.local/bin binaries failed ($RC): $(show)"
for b in planar planar-agent; do
  [[ -f "$H/.local/bin/$b" ]] || fail "uninstall removed ~/.local/bin/$b without --yes on a closed stdin"
  grep -Fq "found $H/.local/bin/$b" "$TMP/out" || fail "uninstall did not name ~/.local/bin/$b"
done
grep -Fq "left them: stdin is not a terminal" "$TMP/out" && grep -Fq -- "--yes" "$TMP/out" || fail "uninstall did not say it left them and how to remove them: $(show)"
in_arena "$H" -- "$ROOT/scripts/uninstall.sh" --yes
[[ "$RC" == 0 ]] || fail "uninstall --yes failed ($RC): $(show)"
[[ ! -e "$H/.local/bin/planar" && ! -e "$H/.local/bin/planar-agent" ]] || fail "uninstall --yes left the ~/.local/bin binaries"
grep -Fq "removed binary $H/.local/bin/planar" "$TMP/out" || fail "uninstall --yes did not name the removal"
[[ "$(cat "$H/.local/bin/other-tool")" == "not planar" ]] || fail "uninstall --yes removed a file that is not a Planar binary"
pass "~/.local/bin binaries are named and left on a closed stdin, removed with --yes; other files stay"
fi

if scen nomanifest; then
# --- no manifest, or a version 1 manifest: nothing under the vendors ---------------------------

for kind in none v1; do
  H="$(make_installed "nomanifest-$kind")"; P="$H/.planar"
  case "$kind" in
    none) rm -f "$P/install-manifest.json" ;;
    v1) printf '{\n  "version": 1,\n  "build_id": "old",\n  "extras": ["%s"]\n}\n' "$H/.claude/agents/planar-coder.md" > "$P/install-manifest.json" ;;
  esac
  vbefore="$(sums "$H/.claude")$(sums "$H/.codex")"
  dbefore="$(data_sums "$P")"
  uninstall "$H"
  [[ "$RC" == 0 ]] || fail "uninstall with manifest=$kind failed ($RC): $(show)"
  [[ "$(sums "$H/.claude")$(sums "$H/.codex")" == "$vbefore" ]] || fail "uninstall with manifest=$kind changed a vendor directory"
  grep -Fq "nothing under the vendor directories was removed" "$TMP/out" || fail "uninstall with manifest=$kind did not say it removed nothing under the vendors: $(show)"
  [[ "$(data_sums "$P")" == "$dbefore" ]] || fail "uninstall with manifest=$kind changed a data path"
  [[ ! -e "$P/bin" ]] || fail "uninstall with manifest=$kind left bin/"
done
# A root holding only a preserved planar.db, no manifest.
H="$(new_home dbonly)"; P="$H/.planar"
mkdir -p "$P" "$H/.claude/agents"
printf 'SQLite format 3\n' > "$P/planar.db"
printf 'mine\n' > "$H/.claude/agents/planar-coder.md"
in_arena "$H" -- "$ROOT/scripts/uninstall.sh"
[[ "$RC" == 0 && "$(cat "$P/planar.db")" == "SQLite format 3" && -f "$H/.claude/agents/planar-coder.md" ]] \
  || fail "uninstall of a root holding only planar.db did not exit 0 keeping the database and the vendor file ($RC): $(show)"
grep -Fq "no install-manifest.json" "$TMP/out" || fail "the no-manifest case was not reported"
pass "with no manifest or a version 1 manifest nothing under the vendor directories is touched, the data is kept, exit 0"
fi

if scen reloc; then
# --- a relocated database is left where it is, --purge included ---------------------------------

H="$(new_home reloc)"; P="$H/.planar"; EXT="$H/ext"
mkdir -p "$EXT"
install_ok "$H" "PLANAR_DB=$EXT/p.db"
printf 'SQLite format 3\nexternal\n' > "$EXT/p.db"; printf 'ext wal\n' > "$EXT/p.db-wal"
mkdir -p "$P/workbench/x"; printf 'readme\n' > "$P/workbench/x/README.md"
ext_before="$(sums "$EXT")"; outside_before="$(sums "$H/work")"
uninstall "$H" "PLANAR_DB=$EXT/p.db" -- --purge
[[ "$RC" == 0 ]] || fail "--purge with a relocated database failed ($RC): $(show)"
grep -Fq "kept relocated data path planar.db at $EXT/p.db (relocated by PLANAR_DB)" "$TMP/out" || fail "--purge did not name the relocated database: $(show)"
[[ "$(sums "$EXT")" == "$ext_before" && "$(sums "$H/work")" == "$outside_before" ]] || fail "--purge touched something outside the install root"
grep -Fq "purging data path $P/workbench" "$TMP/out" && grep -Fq "purging data path $P/templates" "$TMP/out" || fail "--purge did not remove the in-tree data paths: $(show)"
[[ ! -e "$P" ]] || fail "--purge left the install root: $(ls -A "$P" | tr '\n' ' ')"
pass "--purge names a relocated database and leaves it, removes nothing outside the root, removes the in-tree data paths"
fi

if scen purgecfg-keys || scen purgecfg-spellings || scen purgecfg-outside; then
# --- --purge keeps the config that relocates data inside the root -------------------------------
# A config.toml key relocating the workbench or templates into a managed subtree: the uninstall
# leaves that subtree (it holds a data path), so --purge must leave the config too, or the next
# install no longer sees the relocation and replaces the subtree with the data in it.

if scen purgecfg-keys; then
for key in workbench.root templates.dir; do
  case "$key" in workbench.root) sect=workbench; k=root ;; *) sect=templates; k=dir ;; esac
  H="$(new_home "purgecfg-$sect")"; P="$H/.planar"
  install_ok "$H"
  mkdir -p "$P/agents/relocated/sub"
  printf 'plan notes\n' > "$P/agents/relocated/sub/notes.md"; printf 'two\n' > "$P/agents/relocated/b.txt"
  printf '[%s]\n%s = "%s"\n' "$sect" "$k" "$P/agents/../agents/relocated/" > "$P/config.toml"
  data_before="$(sums "$P/agents/relocated")"
  uninstall "$H" -- --purge
  [[ "$RC" == 0 ]] || fail "--purge with $key under a managed tree failed ($RC): $(show)"
  [[ -f "$P/config.toml" ]] || fail "--purge removed config.toml although $key relocates data inside the install root: $(show)"
  grep -Fq "kept $P/config.toml" "$TMP/out" && grep -Fq "config.toml ($key)" "$TMP/out" || fail "--purge did not name the kept config.toml and the relocation ($key): $(show)"
  [[ "$(sums "$P/agents/relocated")" == "$data_before" ]] || fail "--purge changed the relocated $key data"
  install "$H"
  [[ "$RC" != 0 ]] || fail "the install after --purge replaced the subtree holding the relocated $key data: $(show)"
  grep -Fq "refusing to replace" "$TMP/err" && grep -Fq "agents" "$TMP/err" && grep -Fq "data path '$sect'" "$TMP/err" || fail "the following install did not report the relocation into agents ($key): $(show)"
  [[ "$(sums "$P/agents/relocated")" == "$data_before" ]] || fail "the following install changed the relocated $key data"
done
fi
# Spellings that only canonicalisation resolves: a symlink into a managed subtree and a ~ path
# land inside the root (kept); a .. that climbs out of the root lands outside it (removed).
if scen purgecfg-spellings; then
for sp in symlink tilde escape; do
  H="$(new_home "purgecfg-sp-$sp")"; P="$H/.planar"
  install_ok "$H"
  case "$sp" in
    symlink) mkdir -p "$P/agents/relocated"; ln -s "$P/agents" "$H/lnk"; spell="$H/lnk/relocated"; dir="$P/agents/relocated" ;;
    tilde) mkdir -p "$P/agents/relocated"; spell="~/.planar/agents/relocated"; dir="$P/agents/relocated" ;;
    escape) mkdir -p "$H/ext-wb"; spell="$P/agents/../../ext-wb"; dir="$H/ext-wb" ;;
  esac
  printf 'plan notes\n' > "$dir/notes.md"; printf 'two\n' > "$dir/b.txt"
  printf '[workbench]\nroot = "%s"\n' "$spell" > "$P/config.toml"
  data_before="$(sums "$dir")"
  uninstall "$H" -- --purge
  [[ "$RC" == 0 ]] || fail "--purge with a $sp-spelled workbench.root failed ($RC): $(show)"
  if [[ $sp == escape ]]; then
    [[ ! -e "$P/config.toml" ]] || fail "--purge kept config.toml for a .. spelling that escapes the root ($spell)"
  else
    [[ -f "$P/config.toml" ]] || fail "--purge removed config.toml for the $sp spelling of a workbench.root inside the root ($spell): $(show)"
    grep -Fq "kept $P/config.toml" "$TMP/out" && grep -Fq "config.toml (workbench.root)" "$TMP/out" || fail "--purge did not name the kept config.toml for the $sp spelling: $(show)"
    install "$H"
    [[ "$RC" != 0 ]] || fail "the install after --purge replaced the subtree holding the $sp-spelled workbench data: $(show)"
    grep -Fq "refusing to replace" "$TMP/err" && grep -Fq "data path 'workbench'" "$TMP/err" || fail "the following install did not report the relocation for the $sp spelling: $(show)"
  fi
  [[ "$(sums "$dir")" == "$data_before" ]] || fail "the $sp-spelled workbench data changed"
done
fi
# A relocation outside the root, or into a sibling that shares only the root's name as a prefix,
# is not inside it: --purge removes config.toml as before.
if scen purgecfg-outside; then
for n in outside sibling; do
  H="$(new_home "purgecfg-$n")"; P="$H/.planar"
  if [[ $n == outside ]]; then rel="$H/ext-wb"; else rel="${P}2/wb"; fi
  mkdir -p "$rel"; printf 'keep\n' > "$rel/f"
  install_ok "$H"
  printf '[workbench]\nroot = "%s"\n' "$rel" > "$P/config.toml"
  uninstall "$H" -- --purge
  [[ "$RC" == 0 ]] || fail "--purge with a relocation outside the root failed ($RC): $(show)"
  [[ ! -e "$P/config.toml" ]] || fail "--purge kept config.toml for a relocation outside the root ($rel)"
  [[ "$(cat "$rel/f")" == keep ]] || fail "--purge touched $rel"
done
fi
pass "--purge keeps and names config.toml while workbench.root or templates.dir lies inside the root, the next install refuses to replace the holding subtree, and the data is byte-identical; outside the root it removes config.toml"
fi

if scen held; then
# --- a competing owner is refused before anything is removed ----------------------------------

H="$(make_installed held)"; P="$H/.planar"
before="$(tree_state "$H")"
for args in "" "--purge"; do
  hold_lock "$P" install
  uninstall "$H" -- $args
  [[ "$RC" == 1 ]] || fail "uninstall $args under a held lock was not refused ($RC): $(show)"
  grep -Fq "another Planar install (pid $LOCK_HOLDER)" "$TMP/err" || fail "uninstall $args did not name the lock holder: $(show)"
  [[ "$(tree_state "$H")" == "$before" ]] || fail "uninstall $args under a held lock changed the arena"
  kill -9 "$LOCK_HOLDER"; wait "$LOCK_HOLDER" 2>/dev/null || true
done
pass "uninstall and --purge refuse a held mutation lock, naming its owner, before removing anything"
fi

if scen paused; then
# While an uninstall or a purge is paused mid-removal, an install is refused
# naming it; purging the root never removes the lock, so no second owner gets in.
for args in "" "--purge"; do
  H="$(make_installed "paused${args}")"; P="$H/.planar"
  rm -f "$FAULTDIR"/paused.* "$FAULTDIR"/resume.*
  ( cd "$H/work" && /usr/bin/env -i HOME="$H" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" PLANAR_DB="$P/planar.db" \
      PLANAR_INSTALL_TEST_FAULT_ARMED=test-only PLANAR_INSTALL_TEST_FAULT_DIR="$FAULTDIR" \
      PLANAR_INSTALL_TEST_FAULT=pause@uninstall-before-records \
      /bin/bash "$P/bin/planar-uninstall" $args </dev/null >"$TMP/paused.out" 2>&1 ) &
  UPID=$!; BG+=("$UPID")
  i=0; while [[ ! -e "$FAULTDIR/paused.uninstall-before-records" ]]; do i=$((i + 1)); [[ "$i" -lt 600 ]] || fail "the uninstall never paused: $(cat "$TMP/paused.out")"; sleep 0.1; done
  # The uninstaller's own pid, as its ownership record names it.
  owner_rec="$P.lock/owner.$(ls "$P.lock" | sed -n 's/^owner\.//p' | sort -n | tail -n 1)"
  owner_pid="$(sed -n 's/^pid=//p' "$owner_rec")"
  [[ "$(sed -n 's/^operation=//p' "$owner_rec")" == uninstall && -n "$owner_pid" ]] \
    || fail "the paused uninstall $args does not hold the lock as an uninstall"
  install "$H"
  [[ "$RC" == 1 ]] && grep -Fq "another Planar uninstall (pid $owner_pid)" "$TMP/err" || fail "an install during a paused uninstall $args was not refused naming it ($RC): $(show)"
  : > "$FAULTDIR/resume.uninstall-before-records"
  wait "$UPID" || fail "the paused uninstall $args failed: $(cat "$TMP/paused.out")"
  if [[ -n "$args" ]]; then [[ ! -e "$P" ]] || fail "the paused purge left the root"; fi
  lock_released "$P"
done
pass "a paused uninstall or purge holds the lock throughout: a competing install is refused naming it; the lock survives the purge and is then free"
fi

# --- an interrupted uninstall finishes on retry and never resurrects binaries -------------------

if scen killed || scen verify || scen source-retry; then
# Kill after the installed planar-uninstall (bin/) is removed. The journal says
# uninstalling: an install refuses, naming the retry. The printed retry for a
# release install downloads that release's bundle afresh and runs its
# uninstall.sh; following it (from a file:// release base) finishes the removal.
REL="$TMP/release"
mkdir -p "$REL/download/v1.2.3" "$TMP/pack"
cp -R "$BUNDLE" "$TMP/pack/planar-macos-arm64"
( cd "$TMP/pack" && tar -czf "$REL/download/v1.2.3/planar-macos-arm64.tar.gz" planar-macos-arm64 )
# The release's SHA256SUMS, as the release workflow merges it: one `<hash>  <asset>` record.
sums_write() { # sums_write DIR ASSET -- DIR/SHA256SUMS with ASSET's current checksum
  if command -v sha256sum >/dev/null 2>&1; then ( cd "$1" && sha256sum "$2" > SHA256SUMS ); else ( cd "$1" && shasum -a 256 "$2" > SHA256SUMS ); fi
}
sums_write "$REL/download/v1.2.3" planar-macos-arm64.tar.gz
NETBIN="$TMP/netbin"; mkdir -p "$NETBIN"
for n in "$BASEBIN"/*; do ln -s "$(readlink "$n")" "$NETBIN/${n##*/}"; done
for n in curl tar gzip grep mktemp sha256sum shasum; do f="$(command -v "$n" || true)"; [[ -z "$f" ]] || ln -sf "$f" "$NETBIN/$n"; done
# mktemp and tar log what they were asked to do (RTLOG), then run the real tool: the
# retry's scratch directory is removed on exit, so the log is how a test observes
# where it was made and whether anything was extracted.
RTLOG="$TMP/retry-tools.log"
for n in mktemp tar; do
  real="$(command -v "$n")"; rm -f "$NETBIN/$n"
  printf '#!/bin/bash\nprintf "%%s\\n" "%s $*" >> "%s"\nexec "%s" "$@"\n' "$n" "$RTLOG" "$real" > "$NETBIN/$n"
  chmod +x "$NETBIN/$n"
done
# run_retry HOME TMPD CMD -- run the printed retry CMD as an operator would paste it, with a private TMPDIR.
run_retry() {
  RC=0; : > "$RTLOG"
  ( cd "$1/work" && /usr/bin/env -i HOME="$1" PATH="$NETBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$2" PLANAR_DB="$1/.planar/planar.db" \
      /bin/bash -c "$3" </dev/null >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
fi
if scen killed; then
H="$(make_installed killed)"; P="$H/.planar"
recorded "$H" > "$TMP/recorded.killed"
dbefore="$(data_sums "$P")"
uninstall "$H" "PLANAR_RELEASE_URL=file://$REL" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-subtree:bin" --
[[ "$RC" -ge 128 ]] || fail "the uninstall was not killed ($RC): $(show)"
[[ ! -e "$P/bin" && -d "$P/skills" ]] || fail "the kill did not land after bin/ was removed: $(ls -A "$P" | tr '\n' ' ')"
[[ "$(sed -n 's/^phase=//p' "$P/.planar-journal")" == uninstalling ]] || fail "the killed uninstall left no uninstalling journal"
retry="$(sed -n 's/^  if this uninstall is interrupted, finish it with: //p' "$TMP/out")"
[[ "$retry" == *"file://$REL/download/v1.2.3"* && "$retry" == *"planar-macos-arm64.tar.gz"* && "$retry" == *"uninstall.sh"* ]] \
  || fail "the durable retry does not download the matching release's uninstaller: $retry"
install "$H"
[[ "$RC" == 1 ]] && grep -Fq "an uninstall of $P was interrupted" "$TMP/err" && grep -Fq "planar-macos-arm64.tar.gz" "$TMP/err" \
  || fail "an install over the interrupted uninstall was not refused naming the retry ($RC): $(show)"
[[ ! -e "$P/bin" ]] || fail "the refused install resurrected bin/"
mkdir -p "$TMP/rt-killed"
run_retry "$H" "$TMP/rt-killed" "$retry"
[[ "$RC" == 0 ]] || fail "following the durable retry failed ($RC): $(show)"
grep -Fq "finishing an interrupted uninstall" "$TMP/out" || fail "the retry did not finish the interrupted uninstall: $(show)"
for n in bin skills agents codex-agents workflows scripts migrations; do [[ ! -e "$P/$n" ]] || fail "the retry left $n/"; done
[[ ! -e "$P/.planar-journal" && ! -e "$P/release.json" ]] || fail "the retry left the journal or release.json"
while IFS= read -r t; do [[ ! -e "$t" ]] || fail "the interrupted uninstall left the recorded target $t"; done < "$TMP/recorded.killed"
[[ "$(data_sums "$P")" == "$dbefore" ]] || fail "the interrupted uninstall changed a data path"
install "$H"
[[ "$RC" == 0 ]] || fail "a fresh install after the finished uninstall failed ($RC): $(show)"
! grep -Fq "resuming" "$TMP/out" || fail "the install resumed something after the uninstall"
pass "a killed uninstall leaves an uninstalling journal; installs refuse naming the durable retry, which downloads the release's uninstaller and finishes; a fresh install then succeeds"
fi

if scen verify; then
# The printed retry verifies before it extracts (test spec 679, "Error -- the
# printed uninstall retry verifies before it extracts"). Kill an uninstall of a
# release install, then serve an archive that no longer matches SHA256SUMS: the
# retry refuses naming the asset and extracts nothing; the install is as the
# kill left it. A SHA256SUMS without the asset's record refuses the same way.
# With the genuine archive back, the same retry finishes.
H="$(make_installed verify)"; P="$H/.planar"
uninstall "$H" "PLANAR_RELEASE_URL=file://$REL" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-subtree:bin" --
[[ "$RC" -ge 128 && ! -e "$P/bin" && -d "$P/skills" ]] || fail "the verify fixture's uninstall was not killed after bin/ ($RC): $(show)"
retry="$(sed -n 's/^  if this uninstall is interrupted, finish it with: //p' "$TMP/out")"
[[ "$retry" == *"SHA256SUMS"* ]] || fail "the printed release retry does not download SHA256SUMS: $retry"
[[ "$retry" != *"--prebuilt"* ]] || fail "the printed release retry uses --prebuilt: $retry"
ASSET=planar-macos-arm64.tar.gz
cp "$REL/download/v1.2.3/$ASSET" "$TMP/genuine.tar.gz"
mkdir -p "$TMP/pack-bad"; rm -rf "$TMP/pack-bad/planar-macos-arm64"; cp -R "$BUNDLE" "$TMP/pack-bad/planar-macos-arm64"
printf 'tampered\n' > "$TMP/pack-bad/planar-macos-arm64/uninstall.sh"
( cd "$TMP/pack-bad" && tar -czf "$REL/download/v1.2.3/$ASSET" planar-macos-arm64 )
state_before="$(sums "$P")"
mkdir -p "$TMP/rt-mismatch"
run_retry "$H" "$TMP/rt-mismatch" "$retry"
[[ "$RC" != 0 ]] || fail "the retry succeeded against an archive that does not match SHA256SUMS: $(show)"
grep -Fq "checksum verification failed for $ASSET" "$TMP/err" || fail "the retry's refusal does not name the asset: $(show)"
# The scratch directory is gone afterwards, so observe the refusal through the
# tools' log: mktemp ran under TMPDIR (the check can see a scratch directory) and
# tar never ran.
grep -Fq "mktemp -d $TMP/rt-mismatch/planar-uninstall." "$RTLOG" || fail "the retry's scratch directory was not made under TMPDIR, so the extraction check proves nothing: $(cat "$RTLOG")"
! grep -q '^tar ' "$RTLOG" || fail "the retry extracted the archive although it did not match SHA256SUMS: $(cat "$RTLOG")"
[[ -z "$(ls -A "$TMP/rt-mismatch")" ]] || fail "the refused retry left its scratch directory behind: $(find "$TMP/rt-mismatch" | tr '\n' ' ')"
[[ "$(sums "$P")" == "$state_before" ]] || fail "the refused retry changed the installation"
# A SHA256SUMS with no record for the asset.
printf '%064d  other-asset.tar.gz\n' 0 > "$REL/download/v1.2.3/SHA256SUMS"
mkdir -p "$TMP/rt-norecord"
run_retry "$H" "$TMP/rt-norecord" "$retry"
[[ "$RC" != 0 ]] && grep -Fq "checksum verification failed for $ASSET" "$TMP/err" || fail "the retry accepted a SHA256SUMS with no record for the asset ($RC): $(show)"
! grep -q '^tar ' "$RTLOG" || fail "the retry extracted without a checksum record: $(cat "$RTLOG")"
[[ -z "$(ls -A "$TMP/rt-norecord")" ]] || fail "the refused retry left its scratch directory behind: $(find "$TMP/rt-norecord" | tr '\n' ' ')"
# The genuine archive, with its record: the same printed retry finishes.
cp "$TMP/genuine.tar.gz" "$REL/download/v1.2.3/$ASSET"
sums_write "$REL/download/v1.2.3" "$ASSET"
mkdir -p "$TMP/rt-good"
run_retry "$H" "$TMP/rt-good" "$retry"
[[ "$RC" == 0 ]] && grep -Fq "finishing an interrupted uninstall" "$TMP/out" || fail "the retry did not finish once the archive matched SHA256SUMS ($RC): $(show)"
[[ ! -e "$P/skills" && ! -e "$P/.planar-journal" ]] || fail "the verified retry left the installation or the journal"
grep -q '^tar ' "$RTLOG" || fail "the verified retry never extracted, so the refusals above proved nothing: $(cat "$RTLOG")"
[[ -z "$(ls -A "$TMP/rt-good")" ]] || fail "the finished retry left its scratch directory behind: $(find "$TMP/rt-good" | tr '\n' ' ')"
# An https release base never follows a redirect to http; a loopback or file base
# (the fixtures) has no such restriction to impose.
[[ "$retry" != *"--proto-redir"* ]] || fail "the file:// retry carries a redirect restriction it has no use for: $retry"
H2="$(make_installed httpsbase)"
uninstall "$H2" "PLANAR_RELEASE_URL=https://releases.example.test/planar" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-subtree:bin" --
[[ "$RC" -ge 128 ]] || fail "the https-base uninstall was not killed ($RC): $(show)"
retry_https="$(sed -n 's/^  if this uninstall is interrupted, finish it with: //p' "$TMP/out")"
[[ "$(grep -o -e '--proto-redir =https' <<<"$retry_https" | wc -l | tr -d ' ')" == 2 ]] \
  || fail "the retry for an https base does not forbid an https-to-http redirect on both downloads: $retry_https"
# Trailing slashes on the base are dropped, as the bootstrap and install.sh drop them.
H3="$(make_installed slashbase)"
uninstall "$H3" "PLANAR_RELEASE_URL=https://releases.example.test/planar//" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-subtree:bin" --
[[ "$RC" -ge 128 ]] || fail "the slash-base uninstall was not killed ($RC): $(show)"
retry_slash="$(sed -n 's/^  if this uninstall is interrupted, finish it with: //p' "$TMP/out")"
[[ "$retry_slash" == *"u=https://releases.example.test/planar/download/"* ]] \
  || fail "the retry for a base with trailing slashes does not name the base without them: $retry_slash"
pass "the printed release retry downloads SHA256SUMS and refuses a mismatched or unrecorded archive naming the asset, extracting nothing; the genuine archive finishes it"
fi

if scen source-retry; then
# A source install records its checkout, so the installed copy's retry names it
# (quoted: the checkout path holds a space) and completes the uninstall from it.
SRC="$TMP/my checkout"
mkdir -p "$SRC/skills" "$SRC/templates" "$SRC/workflows" "$SRC/migrations"
cp "$ROOT/install.sh" "$SRC/install.sh"
cp -R "$ROOT/scripts" "$SRC/scripts"
rm -rf "$SRC/scripts/__pycache__" "$SRC/scripts/install-lib/__pycache__"
cp -R "$ROOT/agents" "$SRC/agents"
cp -R "$ROOT/skills/planar" "$SRC/skills/planar"
cp "$ROOT/install-cleanup.txt" "$SRC/install-cleanup.txt"
printf 'shipped a\n' > "$SRC/templates/a.toml"
printf -- '-- wf\n' > "$SRC/workflows/w.lua"
printf -- '-- m\n' > "$SRC/migrations/00001_x.up.sql"
: > "$SRC/CMakeLists.txt"; : > "$SRC/CMakePresets.json"
perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang(\+\+)?#/bin/sh#g' "$SRC/install.sh"
SSTUBS="$TMP/sstubs"; mkdir -p "$SSTUBS"
cat > "$SSTUBS/cmake" <<STUB
#!/usr/bin/env bash
if [[ "\$1" == "--install" ]]; then
  source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
  mkdir -p "\$4/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do stub_binary_write "\$4/bin/\$b" "\${STUB_TAG:-dev}"; done
fi
exit 0
STUB
chmod +x "$SSTUBS/cmake"
H="$(new_home source-retry)"; P="$H/.planar"
RC=0
env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB -u PLANAR_CONFIG_PATH PATH="$SSTUBS:$PATH" HOME="$H" NO_COLOR=1 \
  "$SRC/install.sh" --build-dir "$H/build" --no-vendor --prefix "$P" >"$TMP/out" 2>"$TMP/err" || RC=$?
[[ "$RC" == 0 ]] || fail "the source install failed ($RC): $(show)"
grep -Fxq "  \"source_checkout\": \"$SRC\"," "$P/release.json" || fail "the source install did not record its checkout in release.json: $(cat "$P/release.json")"
seed_data "$P"
dbefore="$(data_sums "$P")"
uninstall "$H" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-subtree:bin" --
[[ "$RC" -ge 128 && ! -e "$P/bin" && -d "$P/skills" ]] || fail "the source uninstall was not killed after bin/ ($RC): $(show)"
retry="$(sed -n 's/^  if this uninstall is interrupted, finish it with: //p' "$TMP/out")"
[[ "$retry" == "cd '$SRC' && ./install.sh --uninstall" || "$retry" == "cd ${SRC// /\\ } && ./install.sh --uninstall" ]] \
  || fail "the source install's retry does not name the recorded checkout, quoted: $retry"
mkdir -p "$TMP/rt-source"
run_retry "$H" "$TMP/rt-source" "$retry"
[[ "$RC" == 0 ]] && grep -Fq "finishing an interrupted uninstall" "$TMP/out" || fail "the source retry did not finish the interrupted uninstall ($RC): $(show)"
for n in bin skills agents scripts migrations; do [[ ! -e "$P/$n" ]] || fail "the source retry left $n/"; done
[[ ! -e "$P/.planar-journal" && ! -e "$P/release.json" ]] || fail "the source retry left the journal or release.json"
[[ "$(data_sums "$P")" == "$dbefore" ]] || fail "the source retry changed a data path"
[[ -f "$SRC/install.sh" ]] || fail "the source retry removed the checkout"
pass "a source install records its checkout; the interrupted uninstall's printed retry names it, quoted, and completes the uninstall from it"

# A source install of a TAGGED build writes the tag as release.json's version, so
# the version alone looks like a release. The recorded checkout wins: the printed
# retry is the checkout command, never the release download (which may have no
# asset for this platform).
H="$(new_home source-tagged)"; P="$H/.planar"
RC=0
env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB -u PLANAR_CONFIG_PATH STUB_TAG=v7.8.9 PATH="$SSTUBS:$PATH" HOME="$H" NO_COLOR=1 \
  "$SRC/install.sh" --build-dir "$H/build" --no-vendor --prefix "$P" >"$TMP/out" 2>"$TMP/err" || RC=$?
[[ "$RC" == 0 ]] || fail "the tagged source install failed ($RC): $(show)"
grep -Fxq '  "version": "v7.8.9",' "$P/release.json" && grep -Fxq "  \"source_checkout\": \"$SRC\"," "$P/release.json" \
  || fail "the tagged source install did not record a v-tag version and its checkout: $(cat "$P/release.json")"
uninstall "$H" "PLANAR_RELEASE_URL=file://$REL" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-subtree:bin" --
[[ "$RC" -ge 128 && ! -e "$P/bin" && -d "$P/skills" ]] || fail "the tagged source uninstall was not killed after bin/ ($RC): $(show)"
retry="$(sed -n 's/^  if this uninstall is interrupted, finish it with: //p' "$TMP/out")"
[[ "$retry" == "cd '$SRC' && ./install.sh --uninstall" || "$retry" == "cd ${SRC// /\\ } && ./install.sh --uninstall" ]] \
  || fail "a tagged source install's retry does not name the recorded checkout: $retry"
[[ "$retry" != *".tar.gz"* && "$retry" != *"curl"* ]] || fail "a tagged source install's retry downloads a release: $retry"
mkdir -p "$TMP/rt-tagged"
run_retry "$H" "$TMP/rt-tagged" "$retry"
[[ "$RC" == 0 && ! -e "$P/skills" && ! -e "$P/.planar-journal" ]] || fail "the tagged source retry did not finish the uninstall ($RC): $(show)"
pass "a source install of a tagged build still prints the recorded-checkout retry, not a release download"
fi

if scen killed-vendor; then
# A kill in the middle of the vendor removals: the installed copy is still there
# and the next run finishes every recorded removal.
H="$(make_installed killed-vendor)"; P="$H/.planar"
recorded "$H" > "$TMP/recorded.kv"
uninstall "$H" "PLANAR_INSTALL_TEST_FAULT=kill@uninstall-projection" --
[[ "$RC" -ge 128 && -x "$P/bin/planar-uninstall" ]] || fail "the kill during the vendor removals did not land ($RC): $(show)"
[[ "$(sed -n 's/^phase=//p' "$P/.planar-journal")" == uninstalling ]] || fail "the vendor-stage kill left no uninstalling journal"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "the rerun after the vendor-stage kill failed ($RC): $(show)"
while IFS= read -r t; do [[ ! -e "$t" ]] || fail "the rerun left the recorded target $t"; done < "$TMP/recorded.kv"
pass "a kill during the vendor removals is finished by the next run"
fi

# --- uninstall terminates an interrupted upgrade --------------------------------------------------

if scen upgrade-terminate || scen upgrade-purge; then
upgrade_killed() { # upgrade_killed NAME [ENV=V...] -- an install of v1.2.3 whose upgrade to v1.2.4 is killed mid-swap
  local name="$1"; shift
  H="$(new_home "$name")"; P="$H/.planar"
  install_ok "$H" "$@" --
  seed_data "$P"
  [[ -z "${UPG_EXT-}" ]] || rm -f "$P/planar.db" "$P/planar.db-wal" "$P/planar.db-shm"
  in_arena "$H" "$@" PLANAR_INSTALL_TEST_FAULT=kill@backed-up:workflows -- "$B2/install.sh" --prebuilt "$B2"
  [[ "$RC" -ge 128 ]] || fail "$name: the upgrade was not killed ($RC): $(show)"
  [[ "$(sed -n 's/^phase=//p' "$P/.planar-journal")" == mutating && -d "$P/workflows.old" && ! -e "$P/workflows" ]] \
    || fail "$name: the fixture is not an interrupted upgrade: $(ls -A "$P" | tr '\n' ' ')"
  [[ -n "$(ls -d "$P"/.staging-* 2>/dev/null)" ]] || fail "$name: the interrupted upgrade left no staging"
  # Unknown look-alikes the journal does not own.
  mkdir -p "$P/.staging-not-ours" "$P/research.old"
  printf 'x\n' > "$P/.staging-not-ours/f"; printf 'y\n' > "$P/research.old/f"
  printf 'notes\n' > "$P/notes.txt"
}
fi
if scen upgrade-terminate; then
UPG_EXT=""
upgrade_killed upgrade
owned_staging="$(sed -n 's/^staging=//p' "$P/.planar-journal")"
dbefore="$(data_sums "$P")"
uninstall "$H"
[[ "$RC" == 0 ]] || fail "uninstall over an interrupted upgrade failed ($RC): $(show)"
grep -Fq "ending the pending install" "$TMP/out" || fail "the uninstall did not say it ends the pending install: $(show)"
for s in $owned_staging; do [[ ! -e "$P/$s" ]] || fail "the uninstall left the owned staging $s"; done
[[ ! -e "$P/workflows.old" ]] || fail "the uninstall left the owned backup workflows.old"
grep -Fq "removed the interrupted install's backup $P/workflows.old" "$TMP/out" || fail "the owned backup's removal was not named: $(show)"
for n in bin skills agents codex-agents scripts migrations; do [[ ! -e "$P/$n" ]] || fail "the uninstall left $n/"; done
for e in .staging-not-ours research.old notes.txt; do
  [[ -e "$P/$e" ]] || fail "the uninstall removed the unknown $e"
  grep -Fq "kept $P/$e" "$TMP/err" || fail "the unknown $e was not reported"
done
[[ ! -e "$P/.planar-journal" ]] || fail "the uninstall left the journal"
[[ "$(data_sums "$P")" == "$dbefore" ]] || fail "the uninstall changed a data path"
install "$H"
[[ "$RC" == 0 ]] || fail "a fresh install after cancelling the upgrade failed ($RC): $(show)"
! grep -Fq "resuming the interrupted install" "$TMP/out" || fail "the install replayed the cancelled upgrade"
cmp -s "$BUNDLE/bin/planar" "$P/bin/planar" || fail "the fresh install is not the bundle's release (a cancelled backup was restored?)"
pass "uninstall ends an interrupted upgrade: owned staging and backups go, unknown look-alikes stay and are reported, data stays, a fresh install follows"
fi

if scen upgrade-purge; then
# The same with --purge and a relocated database; the next install, with the
# relocated database, needs no --force.
EXT="$TMP/homes/ext-upgrade"; mkdir -p "$EXT"
printf 'SQLite format 3\nexternal\n' > "$EXT/p.db"
UPG_EXT=1
upgrade_killed upgrade-purge "PLANAR_DB=$EXT/p.db"
ext_before="$(sums "$EXT")"
uninstall "$H" "PLANAR_DB=$EXT/p.db" -- --purge
[[ "$RC" == 0 ]] || fail "--purge over an interrupted upgrade failed ($RC): $(show)"
[[ "$(sums "$EXT")" == "$ext_before" ]] || fail "--purge touched the relocated database"
[[ "$(ls -A "$P" | LC_ALL=C sort | tr '\n' ' ')" == ".planar-uninstalled .staging-not-ours notes.txt research.old " ]] \
  || fail "--purge left more or less than the unknown entries and the marker: $(ls -A "$P" | tr '\n' ' ')"
install "$H" "PLANAR_DB=$EXT/p.db" --
[[ "$RC" == 0 ]] || fail "a fresh install with the relocated database after --purge needed --force ($RC): $(show)"
! grep -Fq "resuming the interrupted install" "$TMP/out" || fail "the install replayed the cancelled upgrade after --purge"
[[ "$(sums "$EXT")" == "$ext_before" ]] || fail "the reinstall changed the relocated database"
pass "--purge ends an interrupted upgrade too, honouring the relocation; the next install adopts the root without --force"
fi

[[ "$PASSED" -gt 0 ]] || fail "group $GROUP ran no check"
printf 'install uninstall tests (group %s%s): %s passed\n' "$GROUP" "${SCEN_FILTER:+, scenario $SCEN_FILTER}" "$PASSED"
