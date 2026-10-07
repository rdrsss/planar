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
#     tag outside the error object are refused; a foreign schema only warns);
#   - --link with --prebuilt exits 2 and a bundle missing a binary (or another
#     required file) exits 1, each before ~/.planar exists;
#   - a source install writes release.json whose version is the sixth token of
#     the binary's version line.
# Everything runs in a scratch HOME against a fake bundle of tiny executables
# (scripts/fixtures/prebuilt-bundle.sh); nothing real is read or built. Set
# PLANAR_REAL_BUNDLE to an unpacked real bundle to add a placement check of its
# binaries. Runs under stock bash 3.2.
# shellcheck disable=SC2016,SC2012  # literal $ in fixtures; ls for messages
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
grep -Fq '0 of 5 binaries written' "$TMP/out" || fail "the second run rewrote binaries: $(cat "$TMP/out")"
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
    [[ "$RC" == 1 ]] || fail "probe case $name: expected a refusal at exit 1, got $RC: $(cat "$TMP/err")"
    grep -Fq 'agent.db was NOT retired' "$TMP/err" || fail "probe case $name: the refusal does not say agent.db was not retired: $(cat "$TMP/err")"
    [[ "$(cat "$p/agent.db")" == old && ! -e "$p/retired" ]] || fail "probe case $name: agent.db moved despite the refusal"
  fi
}
probe_case usable 0 '{"seq":1}' proceeds
probe_case notfound 1 '{"error":{"verb":"queue status","tag":"not_found","message":"m"}}' proceeds
probe_case foreign 125 '{"error":{"verb":"queue status","tag":"queue_schema_foreign","message":"m"}}' proceeds
grep -Fq 'foreign' "$TMP/out" || fail "the foreign verdict was not reported"
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
mkdir -p "$REPO/skills" "$REPO/templates"
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
cat > "$STUBS/cmake" <<'STUB'
#!/usr/bin/env bash
if [[ "$1" == "--install" ]]; then
  prefix="$4"
  mkdir -p "$prefix/bin"
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    printf '#!/bin/sh\nif [ "$1" = version ] && [ "${2:-}" = --json ]; then echo "{\\"release\\":\\"%s\\",\\"sha\\":\\"abc123def4560123456789abcdef0123456789ab\\",\\"date\\":\\"2026-10-06T00:00:00Z\\",\\"dirty\\":false,\\"compiler\\":\\"Clang-23.1.2\\"}"; exit 0; fi\n[ "$1" = version ] && echo "%s abc123def456 2026-10-06T00:00:00Z cxx Clang-23.1.2 %s"\n[ "$1" = queue ] && echo "{\\"seq\\":1}"\nexit 0\n' "${STUB_TAG:-dev}" "$b" "${STUB_TAG:-dev}" > "$prefix/bin/$b"
    chmod +x "$prefix/bin/$b"
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
