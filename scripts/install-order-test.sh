#!/usr/bin/env bash
# The order of an install, end to end with the REAL built binaries (plan 1122,
# task rel-install-order; tech spec 677, "Order of an install" and "Mutation
# ownership and cleanup"; test spec 679, the scenarios that verify
# rel-install-order):
#   - a fresh install creates a current database, registers no project from the
#     cwd, honours a relocated database and config, and health reports no
#     projection drift;
#   - a behind database (one migration down) is migrated after the swap, with
#     its plan and task intact;
#   - a database ahead of the bundle is refused before any change, naming both
#     versions; the arena is byte-identical and no staging or journal is left;
#   - corrupt, same-version foreign and incompatible-queue databases are refused
#     before vendor cleanup or swap; a failed migration and a failed
#     post-migration probe keep the journal and the backups, name the durable
#     retry, and the retry completes once the fault is gone;
#   - running and waiting queue entries are named before the swap, and an
#     absent planar-watch prints one skipped line;
#   - a kill at each point of a subtree swap is recovered by re-running the same
#     command; recovery restores a backup before a failed restaging; unknown
#     *.old and .staging-* entries are kept; files matching no recorded state
#     refuse without deletion;
#   - a second concurrent install is refused naming the holder's pid, and a new
#     run after the holder is killed completes; a killed first installation is
#     recognized without --force; an install killed under one host name is
#     recovered under another;
#   - a refused or failed attempt before any live change leaves the completed
#     install untouched, and a probe failure while recovering keeps the earlier
#     transaction's evidence;
#   - the updater handoff: --cleanup removes exactly the recorded update
#     temporary on success and failure, refuses every other target, and a killed
#     updater's temporary is removed by the next owner;
#   - an uninstalling journal is never replayed, and uninstall ends a pending
#     install so a later install starts fresh.
# Every install runs under /bin/bash with a PATH of install.sh's base tier only
# (no python3, no flock), against a bundle assembled from the built binaries.
# python3 seeds and reads the scratch databases; it is test tooling only.
#
# Usage: install-order-test.sh PLANAR PLANAR-AGENT PLANAR-WATCH PLANAR-EXECUTE PLANAR-EXT
# shellcheck disable=SC2016,SC2012,SC2010,SC2015,SC1091  # literal $ in fixtures; ls for messages; A && B || fail is intended
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -eq 5 ]] || { printf 'usage: install-order-test.sh PLANAR PLANAR-AGENT PLANAR-WATCH PLANAR-EXECUTE PLANAR-EXT\n' >&2; exit 2; }
REAL_BINS=("$@")
for b in "${REAL_BINS[@]}"; do [[ -x "$b" ]] || { printf 'install-order-test: not executable: %s\n' "$b" >&2; exit 2; }; done
PYTHON="$(command -v python3)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
BG=()
cleanup() {
  local p
  for p in ${BG[@]+"${BG[@]}"}; do kill -9 "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  chmod -R u+rwx "$TMP" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT
fail() { printf 'install-order-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); printf 'ok %s %s (%ss)\n' "$PASSED" "$1" "$SECONDS"; SECONDS=0; }
# Test groups (plan 1122 M5, task 7361). The whole file took 757 s on a loaded host, so ctest
# registers one entry per group (install.order_<group>, label install_order_<group>) and no
# entry nears the five-minute ceiling. INSTALL_ORDER_GROUP selects one; unset runs all five
# in file order, as before. An unknown name is a usage error, so a typo cannot pass by
# running nothing.
ORDER_GROUPS="db swap recover concurrent handoff"
GROUP="${INSTALL_ORDER_GROUP:-all}"
case " all $ORDER_GROUPS " in *" $GROUP "*) ;; *) printf 'install-order-test: unknown INSTALL_ORDER_GROUP %s (want one of: %s)\n' "$GROUP" "$ORDER_GROUPS" >&2; exit 2 ;; esac
# Scenario selection and parallel dispatch (plan 1122 M6, task 7434; scripts/fixtures/scenario-runner.sh).
# INSTALL_TEST_SCENARIO=<name> runs only that scenario, inside or outside its group; an unknown
# name exits 2. A run that selects several scenarios runs INSTALL_TEST_JOBS of them at a time
# (default 4), each as a child of this script with its own scratch directory and homes. The table
# is the dispatch order, longest first, name:group. The kill points of the swap scenario are separate names
# (swap-bin for the first, swap-<point> for each point of its loop). The serial scenarios hold a
# mutation lock from a second process while another run is refused, or wait on a paused
# installer, and run alone after the parallel batch (decision 1328: the lock serializes mutation
# of one root, and these scenarios test that serialization).
SCEN_TABLE="recover-journaled:recover refused:concurrent recover-restage:recover updater:handoff faults:db uninstall-pending:handoff queue:db swap-complete:swap swap-bin:swap migfail:db swap-backup-bin:swap swap-swap-skills:swap postprobe:db swap-swap-bin:swap swap-after-mutating:swap roparent:concurrent ahead:db behind:db fresh:db relocated:db durable:db firstkill:concurrent inert:handoff concurrent-refused:concurrent renamed:concurrent uninstall-refused:handoff"
SCEN_SERIAL=" concurrent-refused renamed uninstall-refused "
# shellcheck source=fixtures/scenario-runner.sh
source "$ROOT/scripts/fixtures/scenario-runner.sh"
scen_init install-order-test "$SCEN_TABLE" "$SCEN_SERIAL"
# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"

# --- the PATH: install.sh's base tier only ------------------------------------------

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
# The installer itself runs under bash; a printed retry command names it.
ln -s /bin/bash "$BASEBIN/bash"

# --- bundles of the real binaries ------------------------------------------------------

HEAD_SCHEMA="$(ls "$ROOT/migrations" | sed -n 's/^0*\([0-9][0-9]*\)_.*\.up\.sql$/\1/p' | sort -n | tail -1)"
# The head migration's down file, for the scenarios that put a database one migration behind.
down="$(ls "$ROOT/migrations"/*.down.sql | sort | tail -1)"
# release_json_write DEST TAG -- DEST/release.json for the head schema.
release_json_write() {
  printf '{\n  "version": "%s",\n  "sha": "%s",\n  "date": "2026-10-06T00:00:00Z",\n  "os": "macos",\n  "arch": "arm64",\n  "os_floor": "26.0",\n  "schema_version": %s\n}\n' \
    "$2" "0123456789012345678901234567890123456789" "$HEAD_SCHEMA" > "$1/release.json"
}
# make_bundle DIR TAG -- the repository's bundle layout around the built binaries.
make_bundle() {
  local dest="$1" tag="$2" i names=(planar planar-agent planar-watch planar-execute planar-ext)
  fake_bundle_make "$ROOT" "$dest"
  for i in 0 1 2 3 4; do cp -f "${REAL_BINS[$i]}" "$dest/bin/${names[$i]}"; chmod 755 "$dest/bin/${names[$i]}"; done
  release_json_write "$dest" "$tag"
}
# copy_bundle SRC DEST TAG -- another release of an already staged bundle: a copy of it with its
# own release.json, instead of staging the layout and the binaries again.
copy_bundle() {
  mkdir -p "$(dirname "$2")"
  cp -R "$1" "$2"
  release_json_write "$2" "$3"
}
if [[ -n "${INSTALL_TEST_SHARED-}" ]]; then
  # A parallel child: the dispatching run staged the three bundles; they are read-only here.
  B1="$INSTALL_TEST_SHARED/bundles/v1/planar-bundle"
  B2="$INSTALL_TEST_SHARED/bundles/v2/planar-bundle"
  B3="$INSTALL_TEST_SHARED/bundles/dev/planar-bundle"
else
  B1="$TMP/bundles/v1/planar-bundle"; make_bundle "$B1" v1.2.3
  B2="$TMP/bundles/v2/planar-bundle"; copy_bundle "$B1" "$B2" v1.2.4
  # A development bundle has no published bootstrap: its durable retry is the
  # bundle's own installer (this one is not under an updater's temporary directory).
  B3="$TMP/bundles/dev/planar-bundle"; copy_bundle "$B1" "$B3" dev
fi
scen_dispatch "$TMP/bundles" "$@"

# --- arena helpers ---------------------------------------------------------------------------

FAULTDIR="$TMP/fault"
mkdir -p "$FAULTDIR"
# new_home NAME -- a scratch HOME with Claude present and a git checkout as cwd.
new_home() {
  local h="$TMP/homes/$1"
  mkdir -p "$h/.claude" "$h/work"
  printf '%s' "$h"
}
# run_install HOME BUNDLE [ENV=V...] -- [ARGS...]: run the bundle's installer in
# the foreground; RC, $TMP/out, $TMP/err. FAULT=<spec> arms the test hook.
run_install() {
  local home="$1" bundle="$2"; shift 2
  local envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  [[ "${1-}" == "--" ]] && shift
  RC=0
  ( cd "$home/work" && /usr/bin/env -i HOME="$home" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
      PLANAR_INSTALL_TEST_FAULT_ARMED=test-only PLANAR_INSTALL_TEST_FAULT_DIR="$FAULTDIR" \
      ${envs[@]+"${envs[@]}"} /bin/bash "$bundle/install.sh" --prebuilt "$bundle" "$@" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
show() { printf -- '--- stdout\n%s\n--- stderr\n%s\n' "$(cat "$TMP/out")" "$(cat "$TMP/err")"; }
# py SCRIPT ARGS... -- test-only sqlite helper.
py() { "$PYTHON" -c "$@"; }
db_version() { py 'import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute("select max(version) from schema_migrations").fetchone()[0])' "$1"; }
db_exec() { py 'import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.executescript(sys.argv[2]); c.commit()' "$1" "$2"; }
db_rows() { py 'import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute(sys.argv[2]).fetchall())' "$1" "$2"; }
# planar_at DB ARGS... -- the real planar from /, on a scratch database.
mkdir -p "$TMP/nohome"
planar_at() { local db="$1"; shift; ( cd / && env -i HOME="$TMP/nohome" PATH=/usr/bin:/bin PLANAR_DB="$db" PLANAR_CONFIG_PATH="$TMP/nohome/c.toml" "${REAL_BINS[0]}" "$@" </dev/null ); }
# tree_sum DIR -- every path and file checksum under DIR, planar.db excluded.
tree_sum() { ( cd "$1" && find . ! -name 'planar.db*' -print | LC_ALL=C sort && find . -type f ! -name 'planar.db*' -exec cksum {} + | LC_ALL=C sort ); }
bin_sum() { ( cd "$1" && cksum planar planar-agent planar-watch planar-execute planar-ext ); }
no_evidence() { # no_evidence ROOT -- no journal, staging or backup is left.
  [[ ! -e "$1/.planar-journal" && -z "$(ls -d "$1"/.staging-* "$1"/*.old 2>/dev/null)" ]] \
    || fail "recovery evidence was left in $1: $(ls -a "$1" | tr '\n' ' ')"
}
journal_phase() { sed -n 's/^phase=//p' "$1/.planar-journal" 2>/dev/null; }
# healthy HOME -- a completed install of B1 over a current database.
healthy() {
  local h; h="$(new_home "$1")"
  run_install "$h" "$B1" --
  [[ "$RC" == 0 ]] || fail "$1: the seed install failed ($RC): $(show)"
  printf '%s' "$h"
}

# Hoisted out of the swap group: the recover, concurrent and handoff groups use it too.
# kill_case POINT -- a healthy v1 install, killed at POINT while installing v2.
kill_case() {
  local point="$1" h p old_bin
  h="$(healthy "kill-${point//:/-}")"; p="$h/.planar"
  old_bin="$(bin_sum "$p/bin")"
  run_install "$h" "$B2" PLANAR_INSTALL_TEST_FAULT="kill@$point" --
  [[ "$RC" -ge 128 ]] || fail "kill at $point: the installer was not killed ($RC): $(show)"
  [[ "$(journal_phase "$p")" == mutating ]] || fail "kill at $point: no mutating journal"
  grep -Fq '"version": "v1.2.3"' "$p/release.json" || fail "kill at $point: release.json changed before the end"
  KILL_OLD_BIN="$old_bin"; KILL_HOME="$h"
}
if scen fresh; then
# --- a fresh install creates a current database ---------------------------------------------------

H="$(new_home fresh)"; P="$H/.planar"
( cd "$H/work" && git init -q . 2>/dev/null ) || true
run_install "$H" "$B1" --
[[ "$RC" == 0 ]] || fail "fresh install failed ($RC): $(show)"
[[ -f "$P/planar.db" ]] || fail "a fresh install created no database"
[[ "$(db_version "$P/planar.db")" == "$HEAD_SCHEMA" ]] || fail "the fresh database is not at schema $HEAD_SCHEMA"
[[ "$(db_rows "$P/planar.db" 'select count(*) from projects')" == "[(0,)]" ]] || fail "a project was created from the cwd: $(db_rows "$P/planar.db" 'select * from projects')"
[[ ! -e "$P/config.toml" ]] || fail "the install created config.toml"
health="$(cd / && env -i HOME="$H" PATH=/usr/bin:/bin PLANAR_HOME="$P" PLANAR_DB="$P/planar.db" "$P/bin/planar" health --json </dev/null)"
drift="$(printf '%s' "$health" | py 'import json,sys; h=json.load(sys.stdin); f=h["projection_freshness"]; print(h["schema_current"], f["state"], f["stale"], f["missing"], f["managed"] == f["fresh"])')"
[[ "$drift" == "True fresh 0 0 True" ]] || fail "health after a fresh install: schema/projection drift: $drift ($health)"
no_evidence "$P"
grep -Fq "init --skip-project --allow-no-repo" "$TMP/out" || fail "the initialization command was not printed"
[[ -d "$P.lock" && -z "$(ls "$P.lock" | grep -v '^owner\.\|^released\.' || true)" ]] || fail "the lock directory is not beside the root, holding records only: $(ls -a "$P.lock")"
pass "fresh install creates a current database"
fi

if scen relocated; then
H="$(new_home fresh-relocated)"; P="$H/.planar"
mkdir -p "$H/data" "$H/cfg"
run_install "$H" "$B1" PLANAR_DB="$H/data/elsewhere.db" PLANAR_CONFIG_PATH="$H/cfg/c.toml" --
[[ "$RC" == 0 ]] || fail "relocated fresh install failed ($RC): $(show)"
[[ -f "$H/data/elsewhere.db" && ! -e "$P/planar.db" ]] || fail "the relocated database was not the one created: $(ls -a "$P" "$H/data")"
[[ "$(db_version "$H/data/elsewhere.db")" == "$HEAD_SCHEMA" ]] || fail "the relocated database is not current"
grep -Fq "relocated by PLANAR_DB to $H/data/elsewhere.db" "$TMP/out" || fail "the relocation was not named: $(show)"
pass "fresh install honours a relocated database and config"
fi

if scen durable; then
# The mutating and complete records are flushed to disk with sync(1) once they
# are written: a recording sync on the PATH notes the journal's phase each time
# it runs.
SYNCBIN="$TMP/syncbin"; mkdir -p "$SYNCBIN"
for f in "$BASEBIN"/*; do [[ "${f##*/}" == sync ]] || ln -s "$(readlink "$f")" "$SYNCBIN/${f##*/}"; done
real_sync="$(command -v sync)"
printf '#!/bin/sh\nwhile IFS= read -r l; do case "$l" in phase=*) echo "$l" >> "$SYNC_LOG" ;; esac; done < "$SYNC_ROOT/.planar-journal"\nexec %s\n' "$real_sync" > "$SYNCBIN/sync"
chmod 755 "$SYNCBIN/sync"
H="$(new_home durable)"; P="$H/.planar"
run_install "$H" "$B1" PATH="$SYNCBIN" SYNC_LOG="$TMP/sync.log" SYNC_ROOT="$P" --
[[ "$RC" == 0 ]] || fail "the install with a recording sync failed ($RC): $(show)"
[[ "$(cat "$TMP/sync.log" 2>/dev/null | tr '\n' ' ')" == "phase=mutating phase=complete " ]] \
  || fail "the mutating and complete records were not each flushed with sync: $(cat "$TMP/sync.log" 2>/dev/null)"
pass "the mutating and complete records are flushed with sync"
fi

if scen behind; then
# --- a behind database is migrated after the swap ---------------------------------------------------------

H="$(new_home behind)"; P="$H/.planar"; mkdir -p "$P"
planar_at "$P/planar.db" init --skip-project --allow-no-repo >/dev/null
planar_at "$P/planar.db" plan create "Seed plan" --scope global --json >/dev/null
planar_at "$P/planar.db" task add "Seed task" --plan 1 --scope global --editor=false --json >/dev/null
plan_before="$(planar_at "$P/planar.db" plan show 1 --json)"; task_before="$(planar_at "$P/planar.db" task show 1 --json)"
db_exec "$P/planar.db" "$(cat "$down")"
[[ "$(db_version "$P/planar.db")" == "$((HEAD_SCHEMA - 1))" ]] || fail "the seeded database is not one migration behind"
run_install "$H" "$B1" --
[[ "$RC" == 0 ]] || fail "install over a behind database failed ($RC): $(show)"
[[ "$(db_version "$P/planar.db")" == "$HEAD_SCHEMA" ]] || fail "the behind database was not migrated"
[[ "$(planar_at "$P/planar.db" plan show 1 --json)" == "$plan_before" && "$(planar_at "$P/planar.db" task show 1 --json)" == "$task_before" ]] \
  || fail "the plan or task changed across the migration"
o="$(cat "$TMP/out")"
swap_at="${o%%Swapping the managed subtrees*}"; mig_at="${o%%Migrating the database*}"; cur_at="${o%%is current*}"
[[ "${#swap_at}" -lt "${#mig_at}" && "${#mig_at}" -lt "${#cur_at}" && "${#cur_at}" -lt "${#o}" ]] \
  || fail "the log does not show the swap, then the migration, then a current probe: $(show)"
grep -Fq "behind this release's $HEAD_SCHEMA" "$TMP/out" || fail "the probe did not name the behind versions"
no_evidence "$P"
pass "a behind database is migrated after the swap"
fi

if scen ahead; then
# --- a bundle older than the database is refused before any swap ---------------------------------------------

H="$(healthy ahead)"; P="$H/.planar"
db_exec "$P/planar.db" "insert into schema_migrations (version, description) values ($((HEAD_SCHEMA + 1)), 'newer')"
before_root="$(tree_sum "$P")"; before_db="$(cksum < "$P/planar.db")"; before_vendor="$(tree_sum "$H/.claude")"
run_install "$H" "$B2" --
[[ "$RC" == 1 ]] || fail "an ahead database was not refused at exit 1 ($RC): $(show)"
grep -Fq "at schema $((HEAD_SCHEMA + 1)), newer than this release's schema $HEAD_SCHEMA" "$TMP/err" || fail "the refusal does not name both versions: $(show)"
[[ "$(tree_sum "$P")" == "$before_root" ]] || fail "the refused install changed the installation"
[[ "$(cksum < "$P/planar.db")" == "$before_db" ]] || fail "the refused install changed the database"
[[ "$(tree_sum "$H/.claude")" == "$before_vendor" ]] || fail "the refused install changed a vendor directory"
grep -Fq '"version": "v1.2.3"' "$P/release.json" || fail "release.json changed"
no_evidence "$P"
pass "a bundle older than the database is refused before any swap"
fi

if scen faults; then
# --- database faults have explicit install states ---------------------------------------------------------------

# fault_case NAME SQL-OR-ACTION WANT -- a healthy install, its database damaged;
# the next install is refused before the vendor sweep or any swap.
fault_case() {
  local name="$1" how="$2" want="$3" h p before
  h="$(healthy "fault-$name")"; p="$h/.planar"
  case "$how" in
    corrupt) printf 'this is not a database %.0s' $(seq 1 200) > "$p/planar.db"; rm -f "$p/planar.db-wal" "$p/planar.db-shm" ;;
    *) db_exec "$p/planar.db" "$how" ;;
  esac
  # A previous projection the vendor sweep would retire: it must survive.
  ln -s "$p/agents/claude/old.md" "$h/.claude/agents/old-planar-link.md"
  before="$(tree_sum "$p")"
  run_install "$h" "$B2" --
  [[ "$RC" == 1 ]] || fail "fault $name: not refused ($RC): $(show)"
  grep -Fq "refusing to install: the database $p/planar.db cannot be used" "$TMP/err" || fail "fault $name: the refusal does not name the database: $(show)"
  grep -Fq "$want" "$TMP/err" || fail "fault $name: the original diagnostic '$want' was not kept: $(show)"
  [[ -L "$h/.claude/agents/old-planar-link.md" ]] || fail "fault $name: the vendor sweep ran before the refusal"
  [[ "$(tree_sum "$p")" == "$before" ]] || fail "fault $name: the installation changed"
  no_evidence "$p"
}
fault_case corrupt corrupt "not a database"
fault_case foreign "drop table queue_schema" "queue_schema_foreign"
fault_case incompatible "insert into queue_schema (version, compat, description) values (2, 2, 'needs a newer binary')" "queue store is not usable"
pass "unreadable, foreign and incompatible databases are refused before cleanup or swap"
fi

if scen migfail; then
# A migration whose transaction fails, after the swap: nonzero, rows kept,
# journal and backups kept, the durable retry printed; it completes once fixed.
H="$(healthy migfail)"; P="$H/.planar"
planar_at "$P/planar.db" plan create "Keep me" --scope global --json >/dev/null
db_exec "$P/planar.db" "$(cat "$down")"
# The head migration creates this table; a stray one makes its transaction fail.
blocker="$(sed -n 's/^create table \([a-z_]*\) (.*/\1/p' "$(ls "$ROOT/migrations"/*.up.sql | sort | tail -1)" | head -1)"
[[ -n "$blocker" ]] || fail "could not find a table the head migration creates"
db_exec "$P/planar.db" "create table $blocker (x integer)"
run_install "$H" "$B3" --
[[ "$RC" != 0 ]] || fail "a failed migration reported success: $(show)"
grep -Fq "could not be brought to this release's schema" "$TMP/err" || fail "the migration failure is not named: $(show)"
grep -Fq "is incomplete" "$TMP/err" && grep -Fq "binaries may already have changed" "$TMP/err" || fail "the partial installation is not named: $(show)"
[[ "$(journal_phase "$P")" == mutating ]] || fail "the journal is not kept as mutating: $(cat "$P/.planar-journal" 2>/dev/null)"
[[ -d "$P/bin.old" && -d "$P/skills.old" ]] || fail "the backups were not kept: $(ls -a "$P")"
[[ "$(db_rows "$P/planar.db" "select title from plans")" == "[('Keep me',)]" ]] || fail "existing rows did not survive the failed migration"
retry="$(sed -n '/complete the install with:/{n;s/^  //;p;}' "$TMP/err")"
[[ "$retry" == "bash $B3/install.sh --prebuilt $B3" ]] || fail "the retry command is not the durable bundle command: '$retry'"
db_exec "$P/planar.db" "drop table $blocker"
RC=0
( cd "$H/work" && /usr/bin/env -i HOME="$H" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" /bin/bash -c "$retry" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 0 ]] || fail "the printed retry did not complete the install ($RC): $(show)"
[[ "$(db_version "$P/planar.db")" == "$HEAD_SCHEMA" ]] || fail "the retry did not migrate"
grep -Fq '"version": "dev"' "$P/release.json" || fail "release.json is not the new release after the retry"
no_evidence "$P"
pass "a failed migration keeps its recovery evidence and the printed retry completes it"
fi

if scen postprobe; then
# A post-migration probe failure, and a release tag's retry is the pinned bootstrap.
H="$(healthy postprobe)"; P="$H/.planar"
db_exec "$P/planar.db" "$(cat "$down")"
run_install "$H" "$B2" PLANAR_INSTALL_TEST_FAULT=fail@post-probe --
[[ "$RC" != 0 && "$(journal_phase "$P")" == mutating ]] || fail "a failed post-migration probe did not keep the journal ($RC): $(show)"
grep -Fq "curl -fsSL https://github.com/rdrsss/planar/releases/download/v1.2.4/get-planar.sh | PLANAR_VERSION=v1.2.4 sh" "$TMP/err" \
  || fail "a release install does not name the version-pinned bootstrap: $(show)"
grep -Fq "$TMP" <<< "$(sed -n '/complete the install with:/{n;p;}' "$TMP/err")" && fail "the release retry names a temporary path"
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "re-running after a post-probe failure did not complete ($RC): $(show)"
no_evidence "$P"
pass "a failed post-migration probe keeps the journal; the release retry is the pinned bootstrap"
fi

if scen queue; then
# --- a live queue entry is named before the swap ----------------------------------------------------------------

H="$(healthy queue)"; P="$H/.planar"
db_exec "$P/planar.db" "insert into queue_entries (state,host_id,pid,pid_started,cwd,argv,enqueued_at,refreshed_mono,started_at) values ('running','h',1,1,'/','[\"make\"]',1,1,1);
insert into queue_entries (state,host_id,pid,pid_started,cwd,argv,enqueued_at,refreshed_mono) values ('waiting','h',1,1,'/','[\"make\"]',2,1);"
rows_before="$(db_rows "$P/planar.db" 'select * from queue_entries order by seq')"
seqs="$(db_rows "$P/planar.db" 'select seq from queue_entries order by seq' | tr -dc '0-9,' )"
s1="${seqs%%,*}"; s2="$(printf '%s' "$seqs" | cut -d, -f3)"
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "install with queue entries failed ($RC): $(show)"
grep -Fq "$s1 (running), $s2 (waiting)" "$TMP/err" || fail "the queue entries were not named ($s1, $s2): $(show)"
grep -Fq "can lapse while the database is migrated" "$TMP/err" || fail "the lease warning is missing"
[[ "$(db_rows "$P/planar.db" 'select * from queue_entries order by seq')" == "$rows_before" ]] || fail "the queue rows changed"
# The warning is printed before the swap: the backups do not exist yet when it
# runs, which the order of the step titles shows on stdout.
o="$(cat "$TMP/out")"; pre="${o%%Swapping the managed subtrees*}"
[[ "$pre" == *"Probing the database"* ]] || fail "the probe and warning step does not come before the swap"
rm -f "$P/bin/planar-watch"
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "install with no planar-watch failed ($RC): $(show)"
[[ "$(grep -c 'could not be checked' "$TMP/err")" == 1 ]] || fail "an absent planar-watch did not print exactly one skipped line: $(show)"
pass "live queue entries are named before the swap; an absent planar-watch is one line"
fi

if scen swap-bin || scen swap-backup-bin || scen swap-swap-bin || scen swap-swap-skills || scen swap-after-mutating; then
# --- an interrupted swap is recovered by the next run --------------------------------------------------------------

if scen swap-bin; then
kill_case backed-up:bin
P="$KILL_HOME/.planar"
[[ -d "$P/bin.old" && ! -e "$P/bin" ]] || fail "after the kill bin/ was not moved to bin.old: $(ls -a "$P")"
[[ "$(bin_sum "$P/bin.old")" == "$KILL_OLD_BIN" ]] || fail "bin.old is not the previous bin/"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 0 ]] || fail "the same command did not recover ($RC): $(show)"
[[ "$(bin_sum "$P/bin")" == "$(bin_sum "$B2/bin")" ]] || fail "bin/ is not the bundle's after recovery"
for d in skills agents codex-agents workflows scripts migrations; do
  diff -r "$B2/$d" "$P/$d" >/dev/null 2>&1 || [[ "$d" == agents ]] || fail "$d/ does not match the bundle after recovery"
done
grep -Fq '"version": "v1.2.4"' "$P/release.json" || fail "release.json is not new after recovery"
grep -Fq "restored $P/bin from $P/bin.old" "$TMP/out" || fail "recovery did not restore bin/ before restaging: $(show)"
no_evidence "$P"
fi
for point in backup:bin swap:bin swap:skills after-mutating; do
  scen "swap-${point//:/-}" || continue
  kill_case "$point"
  run_install "$KILL_HOME" "$B2" --
  [[ "$RC" == 0 ]] || fail "recovery after a kill at $point failed ($RC): $(show)"
  [[ "$(bin_sum "$KILL_HOME/.planar/bin")" == "$(bin_sum "$B2/bin")" ]] || fail "kill at $point: bin/ is not the bundle's after recovery"
  no_evidence "$KILL_HOME/.planar"
done
pass "a kill at every swap point is recovered by the same command"
fi

if scen swap-complete; then
# A kill after the commit record leaves only owned cleanup: release.json and the
# stamp are already the new release, and the next run disposes of the backups.
H="$(healthy kill-complete)"; P="$H/.planar"
run_install "$H" "$B2" PLANAR_INSTALL_TEST_FAULT=kill@after-complete --
[[ "$RC" -ge 128 && "$(journal_phase "$P")" == complete ]] || fail "the kill after the commit record did not leave a complete journal ($RC): $(cat "$P/.planar-journal" 2>/dev/null)"
grep -Fq '"version": "v1.2.4"' "$P/release.json" && [[ -f "$P/.planar-install" && -d "$P/bin.old" ]] \
  || fail "the commit record was not written after release.json and the stamp, before the backups were disposed of"
run_install "$H" "$B1" --
[[ "$RC" == 0 ]] || fail "the run after a committed-then-killed install failed ($RC): $(show)"
grep -Fq "the previous install committed; removing its leftover backups and staging" "$TMP/out" || fail "the committed transaction's cleanup was not reported: $(show)"
no_evidence "$P"
pass "a kill after the commit record leaves only owned cleanup for the next run"
fi

if scen recover-restage; then
# --- recovery precedes failed restaging -----------------------------------------------------------------------------

kill_case backed-up:bin
P="$KILL_HOME/.planar"
mkdir -p "$P/foo.old" "$P/.staging-unknown"; printf 'mine\n' > "$P/foo.old/f"; printf 'mine\n' > "$P/.staging-unknown/f"
j_before="$(grep -v '^staging=\|^owner_' "$P/.planar-journal")"
run_install "$KILL_HOME" "$B2" PLANAR_INSTALL_TEST_FAULT=fail@staging --
[[ "$RC" != 0 ]] || fail "an injected staging failure succeeded"
[[ "$(bin_sum "$P/bin")" == "$KILL_OLD_BIN" && ! -e "$P/bin.old" ]] || fail "the previous bin/ was not restored before the failed restaging: $(ls -a "$P")"
[[ "$(journal_phase "$P")" == mutating ]] || fail "a failed restaging aborted the earlier transaction"
[[ -f "$P/foo.old/f" && -f "$P/.staging-unknown/f" ]] || fail "an unknown .old or .staging entry was removed"
grep -Fq "kept $P/foo.old: no recovery journal owns it" "$TMP/err" || fail "the unknown .old entry was not reported: $(show)"
# Fail after several swaps and after the migration; then resume.
run_install "$KILL_HOME" "$B2" PLANAR_INSTALL_TEST_FAULT=fail@after-migrate --
[[ "$RC" != 0 && "$(journal_phase "$P")" == mutating ]] || fail "a failure after the migration did not keep the transaction"
[[ -d "$P/bin.old" && -d "$P/migrations.old" && -d "$P/bin" ]] || fail "live and backups are not both present after a late failure: $(ls -a "$P")"
[[ ! -e "$P/bin/bin.old" && ! -e "$P/bin.old/bin" && ! -e "$P/bin.old/bin.old" ]] || fail "a backup was nested"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 0 ]] || fail "the resume after a late failure did not complete ($RC): $(show)"
[[ "$(bin_sum "$P/bin")" == "$(bin_sum "$B2/bin")" ]] || fail "the resumed install is not the same target"
[[ -f "$P/foo.old/f" && -f "$P/.staging-unknown/f" ]] || fail "completion removed an unknown entry"
[[ ! -e "$P/.planar-journal" && ! -e "$P/bin.old" && -z "$(ls -d "$P"/.staging-[0-9a-f]* 2>/dev/null)" ]] || fail "completion left owned evidence: $(ls -a "$P")"
rm -rf "$P/foo.old" "$P/.staging-unknown"
# A different target while a transaction is pending refuses with its retry.
kill_case backed-up:skills
run_install "$KILL_HOME" "$B1" --
[[ "$RC" == 1 ]] && grep -Fq "finish that one first" "$TMP/err" || fail "a different target did not refuse while a transaction is pending: $(show)"
# Files that match no recorded state refuse without deletion.
P="$KILL_HOME/.planar"
mkdir -p "$P/skills"; printf 'operator\n' > "$P/skills/mine"
before="$(tree_sum "$P")"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 1 ]] && grep -Fq "cannot reconcile skills" "$TMP/err" || fail "an unrecorded state did not refuse: $(show)"
[[ "$(tree_sum "$P")" == "$before" ]] || fail "the reconcile refusal changed the installation"
pass "recovery restores before restaging, keeps unknown entries, resumes after late failures, refuses ambiguity"
fi

if scen recover-journaled; then
# --- a recovery restore is journaled ----------------------------------------------------------------------------------

# A kill right after recovery renames <n>.old back to <n>, before the journal
# records it: the live name holds the recorded inode and no backup is left. The
# same command completes, from a backed_up and from a backing_up interruption.
for first in backed-up:bin backup:bin; do
  kill_case "$first"
  P="$KILL_HOME/.planar"
  run_install "$KILL_HOME" "$B2" PLANAR_INSTALL_TEST_FAULT=kill@restore:bin --
  [[ "$RC" -ge 128 ]] || fail "after $first: the kill at restore:bin did not fire ($RC): $(show)"
  [[ "$(bin_sum "$P/bin")" == "$KILL_OLD_BIN" && ! -e "$P/bin.old" ]] || fail "after $first: bin/ was not restored before the kill: $(ls -a "$P")"
  run_install "$KILL_HOME" "$B2" --
  [[ "$RC" == 0 ]] || fail "after $first: a kill right after the restore was not recovered by the same command ($RC): $(show)"
  [[ "$(bin_sum "$P/bin")" == "$(bin_sum "$B2/bin")" ]] || fail "after $first: bin/ is not the bundle's after recovery"
  no_evidence "$P"
done
# Two subtrees: bin is restored, skills matches no recorded state. The refusal
# names the restore, the journal records it, and once the stray skills/ is gone
# the same command completes.
kill_case backed-up:bin
P="$KILL_HOME/.planar"
# skills/ recorded as being backed up from no live copy; a stray one appeared.
sed -e 's/^sub_skills=.*/sub_skills=backing_up/' -e 's/^sub_skills_live=.*/sub_skills_live=none/' \
    -e 's/^sub_skills_staged=.*/sub_skills_staged=1/' "$P/.planar-journal" > "$TMP/journal.edit"
cat "$TMP/journal.edit" > "$P/.planar-journal"
rm -rf "$P/skills"; mkdir "$P/skills"; printf 'stray\n' > "$P/skills/stray"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 1 ]] && grep -Fq "cannot tell whether $P/skills was backed up" "$TMP/err" || fail "the ambiguous skills/ did not refuse: $(show)"
[[ "$(bin_sum "$P/bin")" == "$KILL_OLD_BIN" && ! -e "$P/bin.old" ]] || fail "bin/ was not restored before the refusal: $(ls -a "$P")"
grep -Fq "this run restored $P/bin from $P/bin.old" "$TMP/err" || fail "the refusal does not name the restore it performed: $(show)"
grep -qx 'sub_bin=pending' "$P/.planar-journal" || fail "the restore was not recorded in the journal: $(cat "$P/.planar-journal")"
rm -rf "$P/skills"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 0 ]] || fail "the rerun after the ambiguity was cleared did not complete ($RC): $(show)"
[[ "$(bin_sum "$P/bin")" == "$(bin_sum "$B2/bin")" ]] && diff -r "$B2/skills" "$P/skills" >/dev/null 2>&1 \
  || fail "the completed rerun is not the bundle's bin/ and skills/"
no_evidence "$P"
pass "a recovery restore is journaled: a kill after it and a later ambiguity both complete on rerun"
fi

if scen concurrent-refused; then
# --- concurrent installs and an interrupted first installation ------------------------------------------------------

H="$(healthy concurrent)"; P="$H/.planar"
rm -f "$FAULTDIR"/*
( cd "$H/work" && exec /usr/bin/env -i HOME="$H" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
    PLANAR_INSTALL_TEST_FAULT_ARMED=test-only PLANAR_INSTALL_TEST_FAULT_DIR="$FAULTDIR" PLANAR_INSTALL_TEST_FAULT=pause@after-lock \
    /bin/bash "$B2/install.sh" --prebuilt "$B2" >"$TMP/holder.out" 2>&1 ) &
HOLDER=$!; BG+=("$HOLDER")
i=0; while [[ ! -e "$FAULTDIR/paused.after-lock" ]]; do i=$((i + 1)); [[ "$i" -lt 600 ]] || fail "the holder never paused: $(cat "$TMP/holder.out")"; sleep 0.1; done
holder_pid="$HOLDER"   # env and the installer exec in place, so this is the installer's pid
before="$(tree_sum "$P")"
run_install "$H" "$B2" --
[[ "$RC" == 1 ]] || fail "a second concurrent install was not refused ($RC): $(show)"
grep -Fq "another Planar install (pid $holder_pid)" "$TMP/err" || fail "the refusal does not name the holder's pid $holder_pid: $(show)"
[[ "$(tree_sum "$P")" == "$before" ]] || fail "the refused concurrent install changed something"
grep -qx "pid=$holder_pid" "$P.lock"/owner.* 2>/dev/null || fail "the paused holder does not own the lock record"
kill -9 "$HOLDER"; wait "$HOLDER" 2>/dev/null || true
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "a run after the holder was killed did not complete ($RC): $(show)"
grep -Fq "reclaimed the mutation lock from an abandoned install pid $holder_pid" "$TMP/out" || fail "the reclaim was not reported: $(show)"
no_evidence "$P"
pass "a second concurrent install is refused; after the holder is killed a new run completes"
fi

if scen firstkill; then
H="$(new_home firstkill)"; P="$H/.planar"
run_install "$H" "$B1" PLANAR_INSTALL_TEST_FAULT=kill@after-prepared --
[[ "$RC" -ge 128 && -f "$P/.planar-journal" && ! -e "$P/bin" && ! -e "$P/.planar-install" && ! -e "$P/planar.db" ]] \
  || fail "the first installation was not killed with only its journal durable ($RC): $(ls -a "$P")"
run_install "$H" "$B1" --
[[ "$RC" == 0 ]] || fail "an interrupted first installation was not recognized without --force ($RC): $(show)"
[[ -x "$P/bin/planar" && -f "$P/.planar-install" && -f "$P/planar.db" ]] || fail "the resumed first installation is incomplete"
no_evidence "$P"
pass "an interrupted first installation is recognized without --force"
fi

if scen renamed; then
# Test spec 679, "Edge — a crashed owner's record survives a hostname change":
# an install KILLed while it holds the lock, the host renamed (only `uname -n`
# changes, as a macOS rename by scutil or DHCP does), and the same command again
# recovers the dead owner and resumes, with nothing removed by hand. The lock
# names the host by its machine identity, which this host must have (a valid
# /etc/machine-id, or the macOS platform UUID); install-lock-test.sh runs the
# same case with a fixture identity on every host.
host_has_identity() {
  local id=""
  if [[ -r /etc/machine-id ]]; then IFS= read -r id < /etc/machine-id || true; fi
  [[ "$id" =~ ^[0-9a-f]{32}$ ]] && return 0
  [[ -x /usr/sbin/ioreg ]] && /usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | grep -q '"IOPlatformUUID" = "'
}
if host_has_identity; then
  real_uname="$(command -v uname)"
  for _nm in host-a host-b; do
    mkdir -p "$TMP/uname-$_nm"
    printf '#!/bin/sh\nif [ "$1" = -n ]; then echo %s.example; exit 0; fi\nexec %s "$@"\n' "$_nm" "$real_uname" > "$TMP/uname-$_nm/uname"
    chmod 755 "$TMP/uname-$_nm/uname"
  done
  H="$(healthy renamed)"; P="$H/.planar"
  run_install "$H" "$B2" "PATH=$TMP/uname-host-a:$BASEBIN" PLANAR_INSTALL_TEST_FAULT=kill@after-mutating --
  [[ "$RC" -ge 128 && "$(journal_phase "$P")" == mutating ]] || fail "the install under the first host name was not killed mid-mutation ($RC): $(show)"
  grep -qx 'node=host-a.example' "$P.lock"/owner.* || fail "the killed install did not record the first host name: $(cat "$P.lock"/owner.*)"
  killed_pid="$(sed -n 's/^pid=//p' "$P.lock"/owner.* | tail -1)"
  run_install "$H" "$B2" "PATH=$TMP/uname-host-b:$BASEBIN" --
  [[ "$RC" == 0 ]] || fail "a killed install was not recovered after a hostname change ($RC): $(show)"
  grep -Fq "reclaimed the mutation lock from an abandoned install pid $killed_pid" "$TMP/out" || fail "the reclaim after the rename was not reported: $(show)"
  grep -Fq 'resuming the interrupted install' "$TMP/out" || fail "the interrupted install was not resumed after the rename: $(show)"
  [[ "$(bin_sum "$P/bin")" == "$(bin_sum "$B2/bin")" ]] || fail "the install resumed after the rename is not the pending target"
  no_evidence "$P"
  pass "a killed install is recovered after a hostname change, without manual removal"
else
  scen_skip "this host has no machine identity; the hostname-change case runs in install-lock-test.sh with a fixture identity"
fi
fi

if scen roparent; then

# --prefix needs a writable parent for <root>.lock: an existing root under a
# parent the operator cannot write refuses before any change, naming the
# parent. A lock directory others can write refuses too.
H="$(new_home roparent)"; P="$H/opt/planar"; mkdir -p "$H/opt"
run_install "$H" "$B1" -- --prefix "$P"
[[ "$RC" == 0 && -d "$P.lock" ]] || fail "the --prefix seed install failed ($RC): $(show)"
rm -rf "$P.lock"; chmod 555 "$H/opt"
before="$(tree_sum "$P")"
run_install "$H" "$B2" -- --prefix "$P"
chmod 755 "$H/opt"
[[ "$RC" == 1 ]] && grep -Fq "cannot create the mutation lock directory $P.lock (is $H/opt writable?)" "$TMP/err" \
  || fail "a root under an unwritable parent did not refuse naming the parent ($RC): $(show)"
[[ "$(tree_sum "$P")" == "$before" ]] || fail "the refusal under an unwritable parent changed the installation"
mkdir "$P.lock"; chmod 770 "$P.lock"
run_install "$H" "$B2" -- --prefix "$P"
[[ "$RC" == 1 ]] && grep -Fq "is writable by its group or by others" "$TMP/err" || fail "a group-writable lock directory was used ($RC): $(show)"
[[ "$(tree_sum "$P")" == "$before" ]] || fail "the refusal over a group-writable lock directory changed the installation"
chmod 700 "$P.lock"
run_install "$H" "$B2" -- --prefix "$P"
[[ "$RC" == 0 ]] || fail "the install after the lock directory was made private failed ($RC): $(show)"
pass "--prefix needs a writable parent for its lock, and a lock directory others can write is refused"
fi

if scen refused; then
# --- a refused attempt does not poison a completed install -----------------------------------------------------------

H="$(healthy refused)"; P="$H/.planar"
stamp="$(cat "$P/.planar-install")"; before="$(tree_sum "$P")"
run_install "$H" "$B2" PLANAR_INSTALL_TEST_FAULT=fail@staging --
[[ "$RC" == 1 ]] || fail "an injected staging failure did not fail ($RC)"
[[ "$(tree_sum "$P")" == "$before" && "$(cat "$P/.planar-install")" == "$stamp" ]] || fail "a failed staging changed the completed install"
grep -Fq 'stopped before changing the installation' "$TMP/err" || fail "the abort was not reported: $(show)"
no_evidence "$P"
run_install "$H" "$B2" PLANAR_INSTALL_TEST_FAULT=kill@after-staging --
[[ "$RC" -ge 128 && "$(journal_phase "$P")" == prepared && -n "$(ls -d "$P"/.staging-* 2>/dev/null)" ]] || fail "the kill did not leave a prepared attempt"
grep -Fq '"version": "v1.2.3"' "$P/release.json" && [[ "$(cat "$P/.planar-install")" == "$stamp" ]] || fail "a prepared attempt changed the completed release"
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "the install after an abandoned prepared attempt failed ($RC): $(show)"
grep -Fq "stopped before changing the installation; removing its staging" "$TMP/out" || fail "recovery did not clean the prepared attempt: $(show)"
grep -Fq '"version": "v1.2.4"' "$P/release.json" || fail "the valid newer upgrade did not complete"
no_evidence "$P"
# A probe failure while recovering an earlier mutating transaction keeps it.
kill_case backed-up:agents
P="$KILL_HOME/.planar"
j_before="$(grep -v '^staging=\|^owner_\|^sub_agents' "$P/.planar-journal")"
run_install "$KILL_HOME" "$B2" PLANAR_INSTALL_TEST_FAULT=fail@probe --
[[ "$RC" == 1 && "$(journal_phase "$P")" == mutating ]] || fail "a probe failure during recovery aborted the transaction ($RC)"
[[ "$(grep -v '^staging=\|^owner_\|^sub_agents' "$P/.planar-journal")" == "$j_before" ]] || fail "the earlier transaction's evidence changed"
[[ -z "$(ls -d "$P"/.staging-* 2>/dev/null | while read -r s; do grep -q "${s##*/}" "$P/.planar-journal" || echo "$s"; done)" ]] || fail "the failed attempt left unrecorded staging"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 0 ]] || fail "recovery after the probe failure did not complete ($RC): $(show)"
no_evidence "$P"
pass "refused and pre-mutation failures leave the completed install authoritative"
fi

if scen updater; then
# --- the update handoff and --cleanup ----------------------------------------------------------------------------------

UPDATER="$TMP/updater.sh"
cat > "$UPDATER" <<'EOS'
# A stand-in for `planar update` (plan 1122 M3): take the lock as the updater,
# record and create its temporary directory, put the bundle there and exec the
# bundled installer with the handoff. $1 root, $2 bundle, $3 cleanup arg
# override (empty: the recorded directory), $4 token override.
root="$1"; bundle="$2"
source "$bundle/scripts/install-lib/mutation-lock.sh"
mkdir -p "$root/.planar-update"
tmp="$root/.planar-update/u$$"
mkdir "$tmp"
planar_lock_acquire "$root" update "$tmp" || { echo "updater: $PLANAR_LOCK_ERROR" >&2; exit 1; }
cp -R "$bundle" "$tmp/bundle"
echo "$tmp" > "$root.updater-tmp"
tok="${4:-$PLANAR_LOCK_GEN:$PLANAR_LOCK_NONCE}"
echo "$tok" > "$root.updater-token"
PLANAR_MUTATION_HANDOFF="$tok" exec /bin/bash "$tmp/bundle/install.sh" --prebuilt "$tmp/bundle" --cleanup "${3:-$tmp}"
EOS
# run_update HOME [CLEANUP-ARG] [TOKEN] [ENV=V...]
run_update() {
  local home="$1" carg="${2:-}" tok="${3:-}"; shift; shift || true; shift || true
  RC=0
  ( cd "$home/work" && /usr/bin/env -i HOME="$home" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" \
      PLANAR_INSTALL_TEST_FAULT_ARMED=test-only PLANAR_INSTALL_TEST_FAULT_DIR="$FAULTDIR" "$@" \
      /bin/bash "$UPDATER" "$home/.planar" "$B2" "$carg" "$tok" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
}
H="$(healthy update)"; P="$H/.planar"
run_update "$H"
[[ "$RC" == 0 ]] || fail "an install handed off by the updater failed ($RC): $(show)"
grep -Fq "adopted the updater's ownership" "$TMP/out" || fail "the handoff was not adopted: $(show)"
[[ ! -e "$(cat "$P.updater-tmp")" ]] || fail "--cleanup did not remove the updater's temporary directory"
[[ ! -e "$P/.planar-update" ]] || fail "the empty update namespace was left"
grep -Fq '"version": "v1.2.4"' "$P/release.json" || fail "the handed-off install did not complete"
g="$(cut -d: -f1 < "$P.updater-token")"
[[ -f "$P.lock/handoff.$g" && -f "$P.lock/released.$g" ]] || fail "the handoff was not consumed and released: $(ls "$P.lock")"
# Installer failure: the temporary directory is removed too.
run_update "$H" "" "" PLANAR_INSTALL_TEST_FAULT=fail@staging
[[ "$RC" == 1 && ! -e "$(cat "$P.updater-tmp")" ]] || fail "an installer failure left the updater's temporary directory ($RC)"
# A replayed handoff refuses and changes nothing.
tok="$(cat "$P.updater-token")"
RC=0
( cd "$H/work" && /usr/bin/env -i HOME="$H" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" PLANAR_MUTATION_HANDOFF="$tok" \
    /bin/bash "$B2/install.sh" --prebuilt "$B2" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 1 ]] && grep -Fq 'refusing the update handoff' "$TMP/err" || fail "a replayed handoff was accepted ($RC): $(show)"
# --cleanup without the handoff, or without --prebuilt.
run_install "$H" "$B2" -- --cleanup "$P/.planar-update/x"
[[ "$RC" == 1 ]] && grep -Fq 'accepted only from the updater' "$TMP/err" || fail "--cleanup without a handoff was accepted ($RC): $(show)"
RC=0; ( /usr/bin/env -i HOME="$H" PATH="$BASEBIN" /bin/bash "$B2/install.sh" --cleanup "$P/x" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 2 ]] || fail "--cleanup without --prebuilt did not exit 2 ($RC)"
# Every target that is not exactly the recorded temporary refuses without deletion.
mkdir -p "$P/workbench/w" "$P/templates" "$TMP/unrelated-tmp" "$TMP/elsewhere"
printf 'w\n' > "$P/workbench/w/f"
ln -s "$TMP/elsewhere" "$P/.planar-update-link"
for target in "$P/workbench" "$P/templates" "$P" "$P/bin" "$TMP/unrelated-tmp" "$P/.planar-update-link" "$P/.planar-update/../bin"; do
  before="$(tree_sum "$P")"
  run_update "$H" "$target"
  [[ "$RC" == 1 ]] || fail "--cleanup $target was accepted ($RC): $(show)"
  grep -Fq "refusing --cleanup $target" "$TMP/err" || fail "--cleanup $target: the refusal does not name it: $(show)"
  [[ -d "$target" ]] || fail "--cleanup $target removed it"
  rm -rf "$(cat "$P.updater-tmp")" "$P/.planar-update"
  [[ "$(tree_sum "$P")" == "$before" ]] || fail "--cleanup $target changed the installation"
done
[[ -f "$P/workbench/w/f" && -d "$TMP/unrelated-tmp" ]] || fail "a refused --cleanup removed a preserved or unrelated path"
# A symlinked ancestor: the namespace itself is a symlink.
mkdir -p "$TMP/ns-real"; ln -s "$TMP/ns-real" "$P/.planar-update"
run_update "$H"
[[ "$RC" == 1 ]] || fail "an update temporary under a symlinked namespace was accepted ($RC): $(show)"
rm -f "$P/.planar-update"
# After a KILL the next owner removes exactly that operation's temporary directory.
mkdir -p "$P/.planar-update/someone-else"
run_update "$H" "" "" PLANAR_INSTALL_TEST_FAULT=kill@after-lock
[[ "$RC" -ge 128 && -d "$(cat "$P.updater-tmp")" ]] || fail "the killed handoff did not leave its temporary directory ($RC)"
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "the install after a killed update failed ($RC): $(show)"
[[ ! -e "$(cat "$P.updater-tmp")" ]] || fail "the abandoned updater's temporary directory was not removed"
[[ -d "$P/.planar-update/someone-else" ]] || fail "an unrelated directory in the update namespace was removed"
grep -Fq "removed the abandoned updater's temporary directory" "$TMP/out" || fail "the reclaim cleanup was not reported: $(show)"
pass "the update handoff, --cleanup containment and killed-updater cleanup"
fi

if scen uninstall-pending; then
# --- uninstalling journals and uninstall over a pending install ------------------------------------------------------

kill_case backed-up:workflows
P="$KILL_HOME/.planar"
RC=0
( /usr/bin/env -i HOME="$KILL_HOME" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C /bin/bash "$B2/install.sh" --uninstall >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 0 ]] || fail "uninstall over a pending install failed ($RC): $(show)"
[[ ! -e "$P/.planar-journal" && ! -e "$P/bin" && ! -e "$P/workflows.old" && -f "$P/planar.db" ]] || fail "uninstall did not end the pending install: $(ls -a "$P")"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 0 ]] || fail "a fresh install after uninstall failed ($RC): $(show)"
! grep -Fq 'resuming the interrupted install' "$TMP/out" || fail "the install replayed a cancelled transaction"
no_evidence "$P"
printf 'planar-journal 1\nroot=%s\nphase=uninstalling\nretry=finish-uninstall-here\n' "$P" > "$P/.planar-journal"
before="$(tree_sum "$P")"
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 1 ]] && grep -Fq 'finish-uninstall-here' "$TMP/err" || fail "an uninstalling journal did not refuse naming its retry: $(show)"
[[ "$(tree_sum "$P")" == "$before" ]] || fail "the refusal over an uninstalling journal changed the installation"
pass "uninstall ends a pending install and an uninstalling journal is never replayed"
fi

if scen uninstall-refused; then
# An uninstall that refuses before removing anything cancels nothing: while
# another process holds the mutation lock the uninstaller refuses naming it, the
# pending install's journal is untouched, and the same install command then
# resumes it once the holder is gone.
# hold_lock ROOT -- a background process that owns ROOT's mutation lock (as an
# install) until it is killed; sets LOCK_HOLDER.
hold_lock() {
  rm -f "$TMP/lock.ready"
  /bin/bash -c 'source "$1"; planar_lock_acquire "$2" install || { echo "$PLANAR_LOCK_ERROR" >&2; exit 1; }; : > "$3"; exec sleep 300' \
    hold "$ROOT/scripts/install-lib/mutation-lock.sh" "$1" "$TMP/lock.ready" &
  LOCK_HOLDER=$!
  BG+=("$LOCK_HOLDER")
  local i=0
  while [[ ! -e "$TMP/lock.ready" ]]; do i=$((i + 1)); [[ "$i" -lt 600 ]] || fail "the lock holder never acquired"; sleep 0.05; done
}
kill_case backed-up:scripts
P="$KILL_HOME/.planar"
j_before="$(cat "$P/.planar-journal")"; before="$(tree_sum "$P")"
hold_lock "$P"
RC=0
( /usr/bin/env -i HOME="$KILL_HOME" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C /bin/bash "$B2/install.sh" --uninstall >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 1 ]] && grep -Fq "another Planar install (pid $LOCK_HOLDER)" "$TMP/err" || fail "uninstall under a held lock did not refuse naming the holder ($RC): $(show)"
[[ "$(cat "$P/.planar-journal")" == "$j_before" ]] || fail "a refused uninstall rewrote the pending install's journal: $(cat "$P/.planar-journal")"
[[ "$(tree_sum "$P")" == "$before" ]] || fail "a refused uninstall changed the installation"
kill -9 "$LOCK_HOLDER"; wait "$LOCK_HOLDER" 2>/dev/null || true
run_install "$KILL_HOME" "$B2" --
[[ "$RC" == 0 ]] || fail "an install after a refused uninstall did not complete ($RC): $(show)"
grep -Fq 'resuming the interrupted install' "$TMP/out" || fail "the pending install was not resumed after the refused uninstall: $(show)"
no_evidence "$P"
# With no pending install, a refused uninstall leaves no journal behind.
H="$(healthy refused-uninstall)"; P="$H/.planar"
hold_lock "$P"
RC=0
( /usr/bin/env -i HOME="$H" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C /bin/bash "$B2/install.sh" --uninstall >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 1 && ! -e "$P/.planar-journal" && -x "$P/bin/planar" ]] || fail "a refused uninstall left a journal or removed something ($RC): $(show)"
kill -9 "$LOCK_HOLDER"; wait "$LOCK_HOLDER" 2>/dev/null || true
run_install "$H" "$B2" --
[[ "$RC" == 0 ]] || fail "an install after a refused uninstall failed ($RC): $(show)"
no_evidence "$P"
pass "a refused uninstall cancels nothing: the pending install resumes and later installs proceed"
fi

if scen inert; then
# --- the fault hook is inert unless armed ---------------------------------------------------------------------------------

H="$(new_home inert)"
RC=0
( cd "$H/work" && /usr/bin/env -i HOME="$H" PATH="$BASEBIN" NO_COLOR=1 LC_ALL=C TMPDIR="$TMP" PLANAR_INSTALL_TEST_FAULT=kill@after-lock \
    /bin/bash "$B1/install.sh" --prebuilt "$B1" >"$TMP/out" 2>"$TMP/err" ) || RC=$?
[[ "$RC" == 0 ]] || fail "the fault hook fired without PLANAR_INSTALL_TEST_FAULT_ARMED ($RC): $(show)"
pass "the test fault hook is inert unless armed"
fi

[[ "$PASSED" -gt 0 ]] || { scen_none_ran; fail "group $GROUP ran no check"; }
printf 'install order tests (group %s%s): %s passed\n' "$GROUP" "${SCEN_FILTER:+, scenario $SCEN_FILTER}" "$PASSED"
