#!/usr/bin/env bash
# Prebuilt-install fixtures (plan 1122, task rel-install-prebuilt; tech spec 677,
# "Prebuilt install mode"). Drives `install.sh --prebuilt <dir>` end to end:
#   - the happy path: bin/, codex-agents/ and release.json are placed from the
#     bundle byte for byte, the vendor surfaces and the version-2 manifest record
#     them, nothing is built, no python3 runs and the PATH holds only the base
#     tier of install.sh's BASE_DEPS (a tool the prebuilt path needs but the tier
#     omits fails the run), and a second run reports no changes;
#   - an old agent.db and its sidecars and old numbered queue logs are moved,
#     unread, into retired/<date>/, colliding names are never overwritten, and
#     the probe in front of it is the shared classifier (concatenated objects, a
#     tag outside the error object and a same-version foreign queue schema are
#     refused before anything changes);
#   - --link with --prebuilt exits 2 and a bundle missing a binary (or another
#     required file) exits 1, each before ~/.planar exists;
#   - a source install writes release.json whose version is the sixth token of
#     the binary's version line.
# Everything runs in a scratch HOME against a fake bundle of tiny executables
# (scripts/fixtures/prebuilt-bundle.sh); nothing real is read or built. Set
# PLANAR_REAL_BUNDLE to an unpacked real bundle to add a placement check of its
# binaries. Runs under stock bash 3.2.
# shellcheck disable=SC2016,SC2012,SC2088  # literal $ in fixtures; ls for messages
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-prebuilt-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); }
# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"

# --- the PATH: the base tier of BASE_DEPS only, plus a recording python3 --------------

BASEBIN="$TMP/basebin"
SHIM="$TMP/shim"
SHIMLOG="$TMP/shim.log"
mkdir -p "$BASEBIN" "$SHIM"
: > "$SHIMLOG"
base_names="$(sed -n '/^BASE_DEPS=(/,/^)/p' "$ROOT/install.sh" | sed -n 's/^  "\([^|"]*\)|.*/\1/p')"
[[ -n "$base_names" ]] || fail "could not read BASE_DEPS from install.sh"
for n in $base_names; do
  found="$(command -v "$n" || true)"
  [[ -n "$found" ]] || fail "base tool $n is not on this host"
  ln -s "$found" "$BASEBIN/$n"
done
for n in cmake ninja python3 python clang clang++; do
  [[ ! -e "$BASEBIN/$n" ]] || fail "$n must not be in the base tier"
done
# A python3 that records its use and fails: the prebuilt path must never call it.
cat > "$SHIM/python3" <<STUB
#!/bin/sh
echo "python3 \$*" >> "$SHIMLOG"
exit 99
STUB
chmod +x "$SHIM/python3"
# The install must not need anything the tier omits. bash itself is /bin/bash.
[[ "$(PATH="$SHIM:$BASEBIN" command -v python3)" == "$SHIM/python3" ]] || fail "the python3 shim is not first on PATH"

# --- bundle and arena ----------------------------------------------------------------

BUNDLE="$TMP/bundle/planar-fake"
fake_bundle_make "$ROOT" "$BUNDLE"

new_home() { # new_home NAME -- a scratch HOME with Claude and Codex present
  local h="$TMP/homes/$1"
  mkdir -p "$h/.claude" "$h/.codex"
  printf '%s' "$h"
}

# run_prebuilt HOME BUNDLE [ENV=V...] -- [install.sh args]: sets RC; output in $TMP/out, $TMP/err.
run_prebuilt() {
  local home="$1" bundle="$2"; shift 2
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  RC=0
  ( cd "$home" && /usr/bin/env -i HOME="$home" PATH="$SHIM:$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
      ${envs[@]+"${envs[@]}"} \
      /bin/bash "${INSTALLER:-$bundle/install.sh}" --prebuilt "$bundle" "$@" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
no_python() { [[ ! -s "$SHIMLOG" ]] || fail "python3 was invoked: $(cat "$SHIMLOG")"; }
tree_sum() { ( cd "$1" && find . -type f | sort | while IFS= read -r f; do cksum "$f"; done ); }

# --- the happy path ------------------------------------------------------------------

H="$(new_home happy)"
P="$H/.planar"
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 ]] || fail "prebuilt install failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
no_python
for b in planar planar-agent planar-watch planar-execute planar-ext; do
  cmp -s "$BUNDLE/bin/$b" "$P/bin/$b" || fail "bin/$b is not byte-identical to the bundle's"
  [[ -x "$P/bin/$b" ]] || fail "bin/$b is not executable"
done
[[ "$(tree_sum "$BUNDLE/codex-agents")" == "$(tree_sum "$P/codex-agents")" ]] || fail "codex-agents/ differs from the bundle's"
cmp -s "$BUNDLE/release.json" "$P/release.json" || fail "release.json differs from the bundle's"
[[ -f "$P/skills/planar/SKILL.md" && -f "$P/agents/planar-coder.md" && -f "$P/templates/a.toml" ]] || fail "staged trees are missing"
[[ -d "$P/migrations" && -f "$P/workflows/w.lua" && -d "$P/scripts/install-lib" ]] || fail "migrations/workflows/scripts were not placed from the bundle"
[[ -f "$H/.claude/skills/planar/SKILL.md" && -f "$H/.claude/agents/planar-coder.md" ]] || fail "the Claude surfaces were not placed"
[[ -f "$H/.codex/agents/planar-coder.toml" ]] || fail "the Codex agent was not placed from the bundled TOML"
cmp -s "$BUNDLE/codex-agents/planar-coder.toml" "$H/.codex/agents/planar-coder.toml" || fail "the Codex agent is not the bundle's pre-rendered TOML"
man="$P/install-manifest.json"
grep -Eq '"version": 2,' "$man" || fail "the manifest is not version 2"
for rec in '"codex-agents/planar-coder.toml"' "\"$H/.claude/agents/planar-coder.md\"" "\"$H/.codex/agents/planar-coder.toml\"" '"build_id": "abc123def456"'; do
  grep -Fq "$rec" "$man" || fail "the manifest does not record $rec"
done
[[ -f "$P/.planar-install" ]] || fail "no ownership stamp"
grep -Fq "prebuilt" "$TMP/out" || fail "the output does not say this is a prebuilt install"
[[ ! -e "$BUNDLE/build" && ! -e "$TMP/build" && ! -e "$H/build" ]] || fail "a build directory appeared"
pass

# A second run reports no changes and leaves the manifest and release.json alone.
man_before="$(cksum < "$man")"; rel_mtime="$(ls -l "$P/release.json" | awk '{print $6 $7 $8}')"
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 ]] || fail "second prebuilt install failed ($RC): $(cat "$TMP/err")"
grep -Eq 'no changes: all [0-9]+ vendor target\(s\) already match' "$TMP/out" || fail "the second run did not report no changes: $(cat "$TMP/out")"
# bin/ is a managed subtree, swapped in whole on every install (tech spec 677,
# "Order of an install"): the second run's binaries are the bundle's again and no
# backup or staging directory is left behind.
for b in planar planar-agent planar-watch planar-execute planar-ext; do
  cmp -s "$BUNDLE/bin/$b" "$P/bin/$b" || fail "the second run left a bin/$b that is not the bundle's"
done
[[ -z "$(ls -d "$P"/*.old "$P"/.staging-* "$P/.planar-journal" 2>/dev/null)" ]] || fail "the second run left recovery evidence: $(ls -a "$P")"
[[ "$(cksum < "$man")" == "$man_before" ]] || fail "the second run changed the manifest"
[[ "$(ls -l "$P/release.json" | awk '{print $6 $7 $8}')" == "$rel_mtime" ]] || fail "the second run rewrote release.json"
no_python
pass

# --dry-run plans, creates nothing.
H="$(new_home dry)"
run_prebuilt "$H" "$BUNDLE" -- --dry-run
[[ "$RC" == 0 ]] || fail "prebuilt dry run failed ($RC): $(cat "$TMP/err")"
[[ ! -e "$H/.planar" ]] || fail "a dry run created ~/.planar"
grep -Fq 'nothing is built' "$TMP/out" || fail "the dry run does not say nothing is built"
pass

# --- the old queue store is moved aside, unread -------------------------------------------

DAY_BEFORE="$(date +%Y-%m-%d)"
H="$(new_home queue)"
P="$H/.planar"
mkdir -p "$P/queue-logs"
printf 'SQLite format 3\000 old queue\n' > "$P/agent.db"
printf 'wal bytes\n' > "$P/agent.db-wal"
printf 'shm bytes\n' > "$P/agent.db-shm"
chmod 000 "$P/agent.db-shm"   # a reader would fail; a move does not read
printf 'log 5\n' > "$P/queue-logs/5.log"
printf 'log 12\n' > "$P/queue-logs/12.log"
printf 'new log\n' > "$P/queue-logs/1000001.log"
printf 'notes\n' > "$P/queue-logs/notes.log"
printf 'txt\n' > "$P/queue-logs/7.txt"
printf 'SQLite format 3\nplanar.db\n' > "$P/planar.db"
# retired/<day>/ already holds an agent.db (a collision) and an older day's files.
mkdir -p "$P/retired/old-day" "$P/retired/$DAY_BEFORE"
printf 'earlier retirement\n' > "$P/retired/$DAY_BEFORE/agent.db"
printf 'older\n' > "$P/retired/old-day/agent.db"
sum_agent="$(cksum < "$P/agent.db")"; sum_wal="$(cksum < "$P/agent.db-wal")"
sum_l5="$(cksum < "$P/queue-logs/5.log")"; sum_l12="$(cksum < "$P/queue-logs/12.log")"
old_day_before="$(tree_sum "$P/retired/old-day")"
run_prebuilt "$H" "$BUNDLE" --
DAY_AFTER="$(date +%Y-%m-%d)"
[[ "$RC" == 0 ]] || fail "install over an old queue store failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
no_python
RD="$P/retired/$DAY_BEFORE"
[[ -d "$RD" ]] || RD="$P/retired/$DAY_AFTER"
for f in agent.db agent.db-wal agent.db-shm; do
  [[ ! -e "$P/$f" && ! -L "$P/$f" ]] || fail "$f is still in the install root"
done
[[ "$(cat "$RD/agent.db")" == "earlier retirement" ]] || fail "an earlier retirement was overwritten"
# SQLite pairs a database with the sidecars named <db>-wal and <db>-shm, so a
# collision on any of the three moves all three under one shared suffix.
[[ "$(cksum < "$RD/agent.db.1")" == "$sum_agent" ]] || fail "the colliding agent.db was not kept as agent.db.1, intact"
[[ "$(cksum < "$RD/agent.db.1-wal")" == "$sum_wal" ]] || fail "agent.db-wal did not move intact beside agent.db.1 as agent.db.1-wal: $(ls "$RD")"
[[ -e "$RD/agent.db.1-shm" ]] || fail "agent.db-shm did not move beside agent.db.1 as agent.db.1-shm: $(ls "$RD")"
[[ ! -e "$RD/agent.db-wal" && ! -e "$RD/agent.db-shm" ]] || fail "a sidecar was split from its database: $(ls "$RD")"
[[ "$(cksum < "$RD/queue-logs/5.log")" == "$sum_l5" && "$(cksum < "$RD/queue-logs/12.log")" == "$sum_l12" ]] || fail "old numbered logs did not move intact"
[[ ! -e "$P/queue-logs/5.log" && ! -e "$P/queue-logs/12.log" ]] || fail "old numbered logs remain in queue-logs/"
[[ -f "$P/queue-logs/1000001.log" && -f "$P/queue-logs/notes.log" && -f "$P/queue-logs/7.txt" ]] || fail "a log that is not an old numbered one was moved"
[[ "$(cat "$P/retired/old-day/agent.db")" == "older" ]] || fail "an older retired file changed"
for line in "moved $P/agent.db -> $RD/agent.db.1" "moved $P/agent.db-wal -> $RD/agent.db.1-wal" "moved $P/queue-logs/5.log -> $RD/queue-logs/5.log"; do
  grep -Fq "$line" "$TMP/out" || fail "the move was not printed: $line
$(cat "$TMP/out")"
done
[[ "$(cat "$P/planar.db" | head -1)" == "SQLite format 3" ]] || fail "planar.db was touched"
pass
chmod 600 "$RD/agent.db.1-shm" 2>/dev/null || true

# A second run moves nothing more; a new agent.db takes the next free suffix.
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 ]] || fail "re-run after a retirement failed ($RC): $(cat "$TMP/err")"
! grep -Fq 'moved ' "$TMP/out" || fail "a re-run moved something: $(cat "$TMP/out")"
printf 'another old store\n' > "$P/agent.db"
printf 'another wal\n' > "$P/agent.db-wal"
run_prebuilt "$H" "$BUNDLE" --
[[ "$RC" == 0 ]] || fail "second retirement failed ($RC): $(cat "$TMP/err")"
[[ "$(cat "$RD/agent.db.2")" == "another old store" && "$(cksum < "$RD/agent.db.1")" == "$sum_agent" ]] \
  || fail "the second collision did not take agent.db.2 and leave agent.db.1 intact"
[[ "$(cat "$RD/agent.db.2-wal")" == "another wal" && "$(cksum < "$RD/agent.db.1-wal")" == "$sum_wal" ]] \
  || fail "the second collision did not keep its wal beside agent.db.2: $(ls "$RD")"
[[ "$(tree_sum "$P/retired/old-day")" == "$old_day_before" ]] || fail "an older retired day changed"
no_python
pass

# --- the probe in front of it is the shared classifier --------------------------------------

# probe_case NAME RC OUT EXPECT -- stub planar-agent's answer; EXPECT is `proceeds` or `refuses`.
probe_case() {
  local name="$1" rc="$2" out="$3" expect="$4" h p
  h="$(new_home "probe-$name")"; p="$h/.planar"
  mkdir -p "$p"
  printf 'SQLite format 3\n' > "$p/planar.db"
  printf 'old\n' > "$p/agent.db"
  printf '%s' "$out" > "$p/stub.out"; printf '%s' "$rc" > "$p/stub.rc"
  run_prebuilt "$h" "$BUNDLE" --
  no_python
  if [[ "$expect" == proceeds ]]; then
    [[ "$RC" == 0 ]] || fail "probe case $name: expected the install to proceed ($RC): $(cat "$TMP/err")"
    [[ ! -e "$p/agent.db" && -f "$(ls -d "$p"/retired/*/ | head -1)agent.db" ]] || fail "probe case $name: agent.db was not moved"
  else
    # The staged binaries' probe refuses before anything live changes: no bin/,
    # no move of agent.db, and no journal or staging left behind.
    [[ "$RC" == 1 ]] || fail "probe case $name: expected a refusal at exit 1, got $RC: $(cat "$TMP/err")"
    grep -Fq 'refusing to install: the database' "$TMP/err" || fail "probe case $name: the refusal does not name the database: $(cat "$TMP/err")"
    grep -Fq 'Nothing was changed' "$TMP/err" || fail "probe case $name: the refusal does not say nothing changed: $(cat "$TMP/err")"
    [[ "$(cat "$p/agent.db")" == old && ! -e "$p/retired" ]] || fail "probe case $name: agent.db moved despite the refusal"
    [[ ! -e "$p/bin" && ! -e "$p/.planar-journal" && -z "$(ls -d "$p"/.staging-* 2>/dev/null)" ]] \
      || fail "probe case $name: the refused attempt left changes: $(ls -a "$p")"
  fi
}
probe_case usable 0 '{"seq":1}' proceeds
probe_case notfound 1 '{"error":{"verb":"queue status","tag":"not_found","message":"m"}}' proceeds
# A same-version foreign queue schema is a database fault: refused before any
# change (tech spec 677 step 5), no longer a warning.
probe_case foreign 125 '{"error":{"verb":"queue status","tag":"queue_schema_foreign","message":"m"}}' refuses
grep -Fq 'foreign' "$TMP/err" || fail "the foreign verdict was not reported"
probe_case concatenated 0 '{"seq":1}{"seq":2}' refuses
probe_case invalid 0 '{garbage}' refuses
probe_case taghoisted 125 '{"error":{"verb":"queue status"},"tag":"queue_schema_foreign"}' refuses
probe_case unknowntag 125 '{"error":{"verb":"queue status","tag":"store_unreachable"}}' refuses
probe_case nested 0 '{"seq":1,"x":{"error":1}}' proceeds
pass

# --- --link is refused with --prebuilt -------------------------------------------------------

H="$(new_home link)"
run_prebuilt "$H" "$BUNDLE" -- --link
[[ "$RC" == 2 ]] || fail "--link with --prebuilt exited $RC, not 2"
grep -Fq -- '--link' "$TMP/err" || fail "the refusal does not name --link: $(cat "$TMP/err")"
grep -Fq -- '--prebuilt' "$TMP/err" || fail "the refusal does not name --prebuilt: $(cat "$TMP/err")"
[[ ! -e "$H/.planar" ]] || fail "--link with --prebuilt created ~/.planar"
no_python
pass

# --- a bundle missing a required file is refused before any write ---------------------------------

for missing in bin/planar bin/planar-agent bin/planar-watch bin/planar-execute bin/planar-ext release.json skills/planar/SKILL.md codex-agents; do
  B="$TMP/broken/$(echo "$missing" | tr / _)"
  mkdir -p "$TMP/broken"
  cp -R "$BUNDLE" "$B"
  rm -rf "${B:?}/$missing"
  H="$(new_home "missing-$(echo "$missing" | tr / _)")"
  run_prebuilt "$H" "$B" --
  [[ "$RC" == 1 ]] || fail "a bundle without $missing exited $RC, not 1"
  grep -Fq "$missing" "$TMP/err" || fail "the refusal does not name $missing: $(cat "$TMP/err")"
  [[ ! -e "$H/.planar" ]] || fail "a bundle without $missing created ~/.planar"
done
# A non-executable binary counts as missing; a missing directory is refused too.
B="$TMP/broken/noexec"; cp -R "$BUNDLE" "$B"; chmod 644 "$B/bin/planar-ext"
H="$(new_home noexec)"; run_prebuilt "$H" "$B" --
[[ "$RC" == 1 ]] && grep -Fq 'bin/planar-ext' "$TMP/err" && [[ ! -e "$H/.planar" ]] || fail "a non-executable binary was not refused cleanly"
H="$(new_home nodir)"; INSTALLER="$BUNDLE/install.sh" run_prebuilt "$H" "$TMP/does-not-exist" --
[[ "$RC" == 1 && ! -e "$H/.planar" ]] || fail "a missing bundle directory was not refused cleanly ($RC)"
pass

# --- a source install writes a release record ---------------------------------------------------------

REPO="$TMP/repo"
mkdir -p "$REPO/skills" "$REPO/templates" "$REPO/workflows"
cp "$ROOT/install.sh" "$REPO/install.sh"
cp -R "$ROOT/scripts" "$REPO/scripts"
rm -rf "$REPO/scripts/install-lib/__pycache__"
cp -R "$ROOT/agents" "$REPO/agents"
cp -R "$ROOT/migrations" "$REPO/migrations"
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
max_migration="$(ls "$ROOT/migrations" | sed -n 's/^0*\([0-9][0-9]*\)_.*\.up\.sql$/\1/p' | sort -n | tail -1)"
[[ -n "$max_migration" ]] || fail "no migrations found"

run_source() { # run_source HOME [ENV=V...]
  local home="$1"; shift
  RC=0
  env -u CODEX_HOME -u PLANAR_HOME -u PLANAR_DB PATH="$STUBS:$PATH" HOME="$home" NO_COLOR=1 "$@" \
    "$REPO/install.sh" --build-dir "$home/build" --no-vendor --prefix "$home/.planar" >"$TMP/out" 2>"$TMP/err" || RC=$?
}
H="$TMP/homes/source-dev"; mkdir -p "$H"
run_source "$H"
[[ "$RC" == 0 ]] || fail "source install failed ($RC): $(cat "$TMP/err")"
R="$H/.planar/release.json"
[[ -f "$R" ]] || fail "a source install wrote no release.json"
grep -Fxq '  "version": "dev",' "$R" || fail "an untagged source build did not record version dev: $(cat "$R")"
for key in sha date os arch os_floor; do grep -Eq "^  \"$key\": \"[^\"]+\",\$" "$R" || fail "release.json lacks $key: $(cat "$R")"; done
grep -Fxq "  \"schema_version\": $max_migration" "$R" || fail "schema_version is not the highest embedded migration ($max_migration): $(cat "$R")"
# The sha is the full 40-character commit from `planar version --json`, not the
# version line's short form, and os_floor is the platform floor scripts/dist.sh
# records (26.0 on macOS, the glibc floor 2.36 on Linux), not the host's version.
grep -Fxq '  "sha": "abc123def4560123456789abcdef0123456789ab",' "$R" || fail "sha is not the full sha from version --json: $(cat "$R")"
case "$(uname -s)" in
  Darwin) want_floor=26.0 ;;
  Linux) want_floor=2.36 ;;
  *) want_floor=unknown ;;
esac
dist_floor="$(sed -n "s/^  $(uname -s)-$(uname -m)) .*floor=\([0-9.]*\) ;;\$/\1/p" "$ROOT/scripts/dist.sh")"
[[ -z "$dist_floor" || "$dist_floor" == "$want_floor" ]] || fail "this test's floor $want_floor disagrees with scripts/dist.sh ($dist_floor)"
grep -Fxq "  \"os_floor\": \"$want_floor\"," "$R" || fail "os_floor is not $want_floor: $(cat "$R")"
H="$TMP/homes/source-tag"; mkdir -p "$H"
run_source "$H" STUB_TAG=v7.8.9
[[ "$RC" == 0 ]] || fail "tagged source install failed ($RC): $(cat "$TMP/err")"
grep -Fxq '  "version": "v7.8.9",' "$H/.planar/release.json" || fail "version is not the sixth token: $(cat "$H/.planar/release.json")"
pass

# --- a shadowing planar is named ----------------------------------------------------------
# A planar in ~/.local/bin ahead of ~/.planar/bin on PATH: the install still exits 0 and
# warns, naming ~/.local/bin/planar and the installed path. With ~/.planar/bin first
# there is no such warning.

H="$(new_home shadow)"
mkdir -p "$H/.local/bin"
printf '#!/bin/sh\nexit 0\n' > "$H/.local/bin/planar"
chmod 755 "$H/.local/bin/planar"
run_prebuilt "$H" "$BUNDLE" "PATH=$H/.local/bin:$H/.planar/bin:$SHIM:$BASEBIN" --
[[ "$RC" == 0 ]] || fail "shadowed install failed ($RC): $(cat "$TMP/err")"
grep -F '~/.local/bin/planar' "$TMP/err" | grep -Fq "$H/.planar/bin/planar" \
  || fail "shadow-warning-names-local-bin: no warning names ~/.local/bin/planar and the installed path: $(cat "$TMP/err")"
pass

H="$(new_home noshadow)"
mkdir -p "$H/.local/bin"
printf '#!/bin/sh\nexit 0\n' > "$H/.local/bin/planar"
chmod 755 "$H/.local/bin/planar"
run_prebuilt "$H" "$BUNDLE" "PATH=$H/.planar/bin:$H/.local/bin:$SHIM:$BASEBIN" --
[[ "$RC" == 0 ]] || fail "unshadowed install failed ($RC): $(cat "$TMP/err")"
if grep -Fq 'shadows' "$TMP/err" "$TMP/out"; then fail "no-warning-when-planar-bin-first: a shadow warning appeared with ~/.planar/bin first"; fi
pass

# --- PLANAR_EXPECT_RECOVERY: recovery is revalidated under the mutation lock ----------------
# The release bootstrap pins an interrupted install and passes its operation id. Under the
# lock the installer refuses (exit 1, nothing changed) when the journal is gone, settled or
# records another operation; the matching journal is recovered. Tech spec 677, "The
# bootstrap" step 2.

kill_mutating() { # kill_mutating NAME -- an install killed after its mutating record; sets KH, KP, KOP
  KH="$(new_home "$1")"; KP="$KH/.planar"
  run_prebuilt "$KH" "$BUNDLE" PLANAR_INSTALL_TEST_FAULT=kill@after-mutating PLANAR_INSTALL_TEST_FAULT_ARMED=test-only -- 
  [[ "$RC" -ge 128 ]] || fail "expect-recovery: the installer was not killed ($RC): $(cat "$TMP/err")"
  grep -Fxq 'phase=mutating' "$KP/.planar-journal" || fail "expect-recovery: no mutating journal: $(cat "$KP/.planar-journal")"
  KOP="$(sed -n 's/^operation_id=//p' "$KP/.planar-journal")"
  [[ "$KOP" =~ ^[0-9a-f]{32}$ ]] || fail "expect-recovery: the journal holds no operation id"
}

# absent journal: refused, nothing changed
kill_mutating expect-absent
rm -f "$KP/.planar-journal"
: > "$KP/planar.db"   # the prefix guard still recognises the root, so the refusal is the lock-time one
before="$(tree_sum "$KP")"
run_prebuilt "$KH" "$BUNDLE" PLANAR_EXPECT_RECOVERY="$KOP" --
[[ "$RC" == 1 ]] || fail "expect-recovery-absent-journal: exit $RC, not 1: $(cat "$TMP/err")"
grep -Fq 'no longer pending' "$TMP/err" || fail "expect-recovery-absent-journal: no refusal message: $(cat "$TMP/err")"
[[ "$(tree_sum "$KP")" == "$before" && ! -e "$KP/release.json" && ! -e "$KP/bin" ]] \
  || fail "expect-recovery-absent-journal: the refused run changed $KP"
pass

# changed journal (another operation id): refused, journal kept, nothing changed
kill_mutating expect-changed
other="$(printf '%s' "$KOP" | tr '0-9a-f' '1-9a-f0')"
[[ "$other" != "$KOP" ]] || fail "expect-recovery: could not derive another operation id"
before="$(tree_sum "$KP")"
run_prebuilt "$KH" "$BUNDLE" PLANAR_EXPECT_RECOVERY="$other" --
[[ "$RC" == 1 ]] || fail "expect-recovery-changed-journal: exit $RC, not 1: $(cat "$TMP/err")"
grep -Fq 'no longer pending' "$TMP/err" || fail "expect-recovery-changed-journal: no refusal message: $(cat "$TMP/err")"
[[ "$(tree_sum "$KP")" == "$before" && ! -e "$KP/release.json" ]] || fail "expect-recovery-changed-journal: the refused run changed $KP"
grep -Fxq 'phase=mutating' "$KP/.planar-journal" || fail "expect-recovery-changed-journal: the journal was not kept"
pass

# a settled journal is not a pending recovery either
kill_mutating expect-settled
sed -i.bak 's/^phase=mutating$/phase=complete/' "$KP/.planar-journal" && rm -f "$KP/.planar-journal.bak"
run_prebuilt "$KH" "$BUNDLE" PLANAR_EXPECT_RECOVERY="$KOP" --
[[ "$RC" == 1 ]] || fail "expect-recovery-settled-journal: exit $RC, not 1: $(cat "$TMP/err")"
[[ ! -e "$KP/release.json" ]] || fail "expect-recovery-settled-journal: the refused run installed"
pass

# the matching journal is recovered, by the very operation named
kill_mutating expect-match
run_prebuilt "$KH" "$BUNDLE" PLANAR_EXPECT_RECOVERY="$KOP" --
[[ "$RC" == 0 ]] || fail "expect-recovery-matching-journal: exit $RC: $(cat "$TMP/err")"
grep -Fq 'resuming the interrupted install' "$TMP/out" "$TMP/err" || fail "expect-recovery-matching-journal: the run did not recover: $(cat "$TMP/out")"
cmp -s "$BUNDLE/release.json" "$KP/release.json" || fail "expect-recovery-matching-journal: release.json is not the bundle's"
pass

# a journal whose ONLY difference is the recorded release base is not the install the
# bootstrap pinned: refused, journal kept, nothing changed (tech spec 677, "The bootstrap"
# step 2; the pin names the release base as well as the operation, tag and commit)
kill_mutating expect-base-changed
grep -Fxq 'release_base=https://github.com/rdrsss/planar/releases' "$KP/.planar-journal" \
  || fail "expect-recovery-base-changed: the journal does not record the default release base: $(cat "$KP/.planar-journal")"
sed -i.bak 's|^release_base=.*|release_base=https://example.invalid/elsewhere|' "$KP/.planar-journal" && rm -f "$KP/.planar-journal.bak"
before="$(tree_sum "$KP")"
run_prebuilt "$KH" "$BUNDLE" PLANAR_EXPECT_RECOVERY="$KOP" --
[[ "$RC" == 1 ]] || fail "expect-recovery-base-changed: exit $RC, not 1: $(cat "$TMP/err")"
grep -Fq 'no longer pending' "$TMP/err" || fail "expect-recovery-base-changed: no refusal message: $(cat "$TMP/err")"
[[ "$(tree_sum "$KP")" == "$before" && ! -e "$KP/release.json" && ! -e "$KP/bin" ]] \
  || fail "expect-recovery-base-changed: the refused run changed $KP"
grep -Fxq 'release_base=https://example.invalid/elsewhere' "$KP/.planar-journal" || fail "expect-recovery-base-changed: the journal was not kept as it was"
pass

# a release base with trailing slashes is the same base: the journal records it without them
# (as the bootstrap compares it) and a rerun with any number of slashes recovers
for slashes in '/' '//'; do
  KH="$(new_home "expect-slash-${#slashes}")"; KP="$KH/.planar"
  run_prebuilt "$KH" "$BUNDLE" "PLANAR_RELEASE_URL=https://example.invalid/rel$slashes" \
    PLANAR_INSTALL_TEST_FAULT=kill@after-mutating PLANAR_INSTALL_TEST_FAULT_ARMED=test-only --
  [[ "$RC" -ge 128 ]] || fail "expect-recovery-slash${slashes}: the installer was not killed ($RC): $(cat "$TMP/err")"
  grep -Fxq 'release_base=https://example.invalid/rel' "$KP/.planar-journal" \
    || fail "expect-recovery-slash${slashes}: the journal does not record the base without trailing slashes: $(grep release_base "$KP/.planar-journal")"
  KOP="$(sed -n 's/^operation_id=//p' "$KP/.planar-journal")"
  # the bootstrap hands the installer its normalized base; the operator may type either form
  for again in 'https://example.invalid/rel' 'https://example.invalid/rel/' 'https://example.invalid/rel//'; do
    cp -R "$KP" "$KH/.planar-copy"
    run_prebuilt "$KH" "$BUNDLE" PLANAR_EXPECT_RECOVERY="$KOP" "PLANAR_RELEASE_URL=$again" --
    [[ "$RC" == 0 ]] || fail "expect-recovery-slash${slashes}-again-$again: exit $RC: $(cat "$TMP/err")"
    grep -Fq 'resuming the interrupted install' "$TMP/out" "$TMP/err" || fail "expect-recovery-slash${slashes}-again-$again: the run did not recover"
    rm -rf "$KP"; mv "$KH/.planar-copy" "$KP"
  done
  pass
done

# grammar and gating: a bad value or a source install refuses before anything is read
H="$(new_home expect-grammar)"
run_prebuilt "$H" "$BUNDLE" 'PLANAR_EXPECT_RECOVERY=$(touch /tmp/x)' --
[[ "$RC" == 2 && ! -e "$H/.planar" ]] || fail "expect-recovery-grammar: exit $RC: $(cat "$TMP/err")"
( cd "$H" && /usr/bin/env -i HOME="$H" PATH="$SHIM:$BASEBIN" PLANAR_EXPECT_RECOVERY=00000000000000000000000000000000 \
    /bin/bash "$ROOT/install.sh" --dry-run >"$TMP/out" 2>"$TMP/err" ) && RC=0 || RC=$?
[[ "$RC" == 2 ]] || fail "expect-recovery-source-install: exit $RC, not 2: $(cat "$TMP/err")"
pass

# --- optional: a real unpacked bundle ---------------------------------------------------------------------

if [[ -n "${PLANAR_REAL_BUNDLE:-}" ]]; then
  [[ -x "$PLANAR_REAL_BUNDLE/bin/planar" ]] || fail "PLANAR_REAL_BUNDLE is not a bundle"
  H="$(new_home real)"
  run_prebuilt "$H" "$PLANAR_REAL_BUNDLE" PLANAR_DB="$H/.planar/planar.db" PLANAR_CONFIG_PATH="$H/.planar/config.toml" --
  [[ "$RC" == 0 ]] || fail "real-bundle install failed ($RC): $(cat "$TMP/err") $(cat "$TMP/out")"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    cmp -s "$PLANAR_REAL_BUNDLE/bin/$b" "$H/.planar/bin/$b" || fail "real bin/$b differs"
  done
  cmp -s "$PLANAR_REAL_BUNDLE/release.json" "$H/.planar/release.json" || fail "real release.json differs"
  pass
fi

printf 'install prebuilt tests: %s passed\n' "$PASSED"
