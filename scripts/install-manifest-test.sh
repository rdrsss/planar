#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/install-manifest.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
HOME="$TMP/home"
PREFIX="$HOME/.planar"
CODEX_HOME="$HOME/.codex"
mkdir -p \
  "$PREFIX/skills/codex/pl-owned" "$PREFIX/codex-skills/pl-owned" \
  "$PREFIX/agents/codex" "$CODEX_HOME/skills/pl-owned" \
  "$CODEX_HOME/skills/local-personal" "$CODEX_HOME/agents"

SOURCE_DIGEST="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
PROJECTION_DIGEST="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
write_projection() {
  local path="$1"
  mkdir -p "$(dirname "$path")"
  printf '%s\n' \
    "# x-planar-source-digest: $SOURCE_DIGEST" \
    "# x-planar-projection-digest: $PROJECTION_DIGEST" \
    "fixture = true" > "$path"
}

# Scriptorium's built-in Codex profile stages skill render output as
# pl-<slug>/SKILL.md (docs/format.md § 2.1's `layout: dir`), not a flat
# pl-<slug>.md file — see install-manifest.sh's codex case.
write_projection "$PREFIX/skills/codex/pl-owned/SKILL.md"
write_projection "$PREFIX/codex-skills/pl-owned/SKILL.md"
write_projection "$PREFIX/agents/codex/coder.toml"
cp "$PREFIX/codex-skills/pl-owned/SKILL.md" "$CODEX_HOME/skills/pl-owned/SKILL.md"
printf 'operator-owned\n' > "$CODEX_HOME/skills/local-personal/SKILL.md"
ln -s "$PREFIX/agents/codex/coder.toml" "$CODEX_HOME/agents/coder.toml"

MANIFEST="$PREFIX/install-manifest.json"
printf '{"old":true}\n' > "$MANIFEST"
install_manifest_begin "build-fixture" copy
install_manifest_add_extra mtkahypar
install_manifest_record_vendor codex "$PREFIX" "$HOME" "$CODEX_HOME"
install_manifest_write "$MANIFEST"

grep -q '"version": 1' "$MANIFEST"
grep -q '"build_id": "build-fixture"' "$MANIFEST"
grep -q '"install_mode": "copy"' "$MANIFEST"
grep -q '"vendors": \["codex"\]' "$MANIFEST"
grep -q '"extras": \["mtkahypar"\]' "$MANIFEST"
[[ "$(grep -o '"vendor": "codex"' "$MANIFEST" | wc -l | tr -d ' ')" -eq 2 ]]
grep -q '"kind": "skill".*"install_kind": "copy"' "$MANIFEST"
grep -q '"kind": "agent".*"install_kind": "link"' "$MANIFEST"
grep -q "\"source_digest\": \"$SOURCE_DIGEST\"" "$MANIFEST"
grep -q "\"projection_digest\": \"$PROJECTION_DIGEST\"" "$MANIFEST"
! grep -q 'claude\|copilot\|local-personal' "$MANIFEST"
[[ ! -e "$MANIFEST.tmp.$$" ]]

# A second successful write atomically replaces the authority and preserves
# the actual per-row copy/link kinds independently of the global install mode.
install_manifest_begin "build-replacement" link
install_manifest_record_vendor codex "$PREFIX" "$HOME" "$CODEX_HOME"
install_manifest_write "$MANIFEST"
grep -q '"build_id": "build-replacement"' "$MANIFEST"
grep -q '"install_mode": "link"' "$MANIFEST"
grep -q '"extras": \[\]' "$MANIFEST"
! grep -q 'build-fixture' "$MANIFEST"
grep -q '"kind": "skill".*"install_kind": "copy"' "$MANIFEST"
grep -q '"kind": "agent".*"install_kind": "link"' "$MANIFEST"
[[ ! -e "$MANIFEST.tmp.$$" ]]

# Digest-less path: what scriptorium actually writes — a SKILL.md with no
# x-planar-source-digest/x-planar-projection-digest headers at all (the
# legacy in-band scheme retired; the in-tree renderer checks staged bytes).
# Confirms install_manifest_add records the row with both digest fields
# empty rather than erroring, and — the actual drift-comparison behavior —
# that the legacy mismatch guard never fires for this case even when the
# staged and installed bytes literally differ, since a digest is compared
# only when the STAGED side has one. A false-positive mismatch here would
# mean scriptorium-rendered installs spuriously fail install.sh.
NODIGEST_PREFIX="$TMP/nodigest_home/.planar"
NODIGEST_CODEX_HOME="$TMP/nodigest_home/.codex"
mkdir -p "$NODIGEST_PREFIX/codex-skills/pl-nodigest" "$NODIGEST_CODEX_HOME/skills/pl-nodigest"
printf 'body = "staged"\n' > "$NODIGEST_PREFIX/codex-skills/pl-nodigest/SKILL.md"
printf 'body = "installed-drifted"\n' > "$NODIGEST_CODEX_HOME/skills/pl-nodigest/SKILL.md"

install_manifest_begin "build-nodigest" copy
install_manifest_add codex skill pl-nodigest \
  "$NODIGEST_PREFIX/codex-skills/pl-nodigest/SKILL.md" \
  "$NODIGEST_CODEX_HOME/skills/pl-nodigest/SKILL.md" \
  copy
NODIGEST_MANIFEST="$TMP/nodigest-manifest.json"
install_manifest_write "$NODIGEST_MANIFEST"
grep -q '"name": "pl-nodigest"' "$NODIGEST_MANIFEST"
grep -q '"source_digest": ""' "$NODIGEST_MANIFEST"
grep -q '"projection_digest": ""' "$NODIGEST_MANIFEST"

# Uninstall preserves the operator's data: `planar.db`, its SQLite sidecars
# and the detached-run output directory `queue-logs/` survive a non-force
# uninstall while every managed artifact is removed. The retired agent store
# `agent.db` and its sidecars are no longer preserved (plan 1089, superseding
# decision 1202): they are removed behind the live-queue guard. The uninstall
# branch returns before the build-dependency preflight, so this drives the
# real `install.sh --uninstall` against a scratch home.
# /bin/bash 3.2 does not honor `set -e` for a failing bare `[[ … ]]`, so
# every assertion below goes through fail() to be non-vacuous.
fail() { printf 'install-manifest-test: %s\n' "$*" >&2; exit 1; }
# make_agent_store PATH [SQL] -- a real, idle agent store (the agent
# migrations' queue tables, no rows) at PATH, plus optional extra SQL. The
# live-queue guard reads it, so a text placeholder would (rightly) be refused.
make_agent_store() {
  python3 - "$1" "${2:-}" <<'PYFIX'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.executescript("""
create table queue_entries (seq integer primary key autoincrement, state text not null, host_id text not null,
  pid integer not null, pid_started integer not null, child_pgid integer, child_started integer, parent_seq integer,
  terminating_since_mono integer, terminate_reason text, cancelled_by text, cwd text not null, argv text not null,
  label text, vendor text, role text, claim_token text, log_path text, enqueued_at integer not null,
  started_at integer, refreshed_mono integer not null, deadline_mono integer, wait_deadline_mono integer,
  run_limit_ms integer, wait_limit_ms integer);
create table queue_history (seq integer primary key, outcome text not null, exit_code integer, signal integer,
  successor_seq integer, cancelled_by text, nested integer not null default 0, parent_seq integer,
  cwd text not null, argv text not null, label text, vendor text, role text, log_path text,
  enqueued_at integer not null, started_at integer, ended_at integer not null, waited_ms integer not null,
  ran_ms integer, run_limit_ms integer, wait_limit_ms integer);
""")
if sys.argv[2]:
    conn.executescript(sys.argv[2])
conn.commit()
conn.close()
PYFIX
}
assert_present() { [[ -e "$1" ]] || fail "expected $1 to survive the uninstall"; }
assert_absent()  { [[ ! -e "$1" ]] || fail "expected $1 to be removed by the uninstall"; }
run_uninstall() {
  local home="$1"; shift
  HOME="$home" PLANAR_HOME="$home/.planar" CODEX_HOME="$home/.codex" \
    "$ROOT/install.sh" --uninstall "$@"
}
seed_prefix() {
  local prefix="$1"
  mkdir -p "$prefix/queue-logs"
  printf 'main db\n' > "$prefix/planar.db"
  printf 'main wal\n' > "$prefix/planar.db-wal"
  printf 'main shm\n' > "$prefix/planar.db-shm"
  make_agent_store "$prefix/agent.db"
  printf 'wal\n' > "$prefix/agent.db-wal"
  printf 'shm\n' > "$prefix/agent.db-shm"
  printf 'detached output\n' > "$prefix/queue-logs/1000001.log"
}

UNINSTALL_HOME="$TMP/uninstall_home"
UNINSTALL_PREFIX="$UNINSTALL_HOME/.planar"
mkdir -p "$UNINSTALL_PREFIX/bin" "$UNINSTALL_PREFIX/skills/src"
seed_prefix "$UNINSTALL_PREFIX"
printf '#!/bin/sh\n' > "$UNINSTALL_PREFIX/bin/planar"
chmod +x "$UNINSTALL_PREFIX/bin/planar"
printf '{"version": 1}\n' > "$UNINSTALL_PREFIX/install-manifest.json"
printf 'skill\n' > "$UNINSTALL_PREFIX/skills/src/pl-owned.md"
run_uninstall "$UNINSTALL_HOME" >"$TMP/uninstall-stdout" 2>"$TMP/uninstall-stderr" \
  || fail "uninstall of a full prefix failed: $(cat "$TMP/uninstall-stderr")"
# Exactly planar.db, its sidecars and queue-logs/ are left; the idle agent.db
# and its sidecars are removed with everything else.
[[ "$(ls -A "$UNINSTALL_PREFIX" | tr '\n' ' ')" == "planar.db planar.db-shm planar.db-wal queue-logs " ]] \
  || fail "uninstall left more or less than planar.db, its sidecars and queue-logs/: $(ls -A "$UNINSTALL_PREFIX" | tr '\n' ' ')"
assert_present "$UNINSTALL_PREFIX/queue-logs/1000001.log"
assert_absent "$UNINSTALL_PREFIX/agent.db"
assert_absent "$UNINSTALL_PREFIX/agent.db-wal"
assert_absent "$UNINSTALL_PREFIX/agent.db-shm"
assert_absent "$UNINSTALL_PREFIX/bin"
assert_absent "$UNINSTALL_PREFIX/skills"
assert_absent "$UNINSTALL_PREFIX/install-manifest.json"

# A prefix whose only would-be ownership signal is agent.db is NOT a Planar
# install any more (plan 1089): the uninstall-side ownership guard refuses it
# without --force and removes nothing.
AGENT_ONLY_HOME="$TMP/agent_only_home"
AGENT_ONLY_PREFIX="$AGENT_ONLY_HOME/.planar"
mkdir -p "$AGENT_ONLY_PREFIX/queue-logs" "$AGENT_ONLY_PREFIX/stale"
make_agent_store "$AGENT_ONLY_PREFIX/agent.db"
printf 'detached output\n' > "$AGENT_ONLY_PREFIX/queue-logs/2.log"
if run_uninstall "$AGENT_ONLY_HOME" >"$TMP/agent-only-stdout" 2>"$TMP/agent-only-stderr"; then
  fail "uninstall accepted a prefix holding only agent.db as a Planar install"
fi
grep -Fq 'does not look like a Planar install' "$TMP/agent-only-stderr" \
  || fail "a prefix holding only agent.db was refused for an unexpected reason: $(cat "$TMP/agent-only-stderr")"
assert_present "$AGENT_ONLY_PREFIX/agent.db"
assert_present "$AGENT_ONLY_PREFIX/stale"

# --force removes the whole prefix, planar.db and its logs included.
FORCE_HOME="$TMP/force_home"
FORCE_PREFIX="$FORCE_HOME/.planar"
seed_prefix "$FORCE_PREFIX"
run_uninstall "$FORCE_HOME" --force >"$TMP/force-stdout" 2>"$TMP/force-stderr" \
  || fail "forced uninstall failed: $(cat "$TMP/force-stderr")"
assert_absent "$FORCE_PREFIX"

# The install-side ownership guard mirrors the uninstall-side one. It sits
# behind the build-dependency preflight, so exercise the exact sourced block
# (from the PLANAR_STAMP assignment through the guard's closing `fi`) with an
# err() that exits 1, the way the installer's does. A prefix holding only a
# foreign file must still be refused, which also proves the extracted block is
# not empty.
sed -n '/^PLANAR_STAMP=/,/^fi$/p' "$ROOT/install.sh" > "$TMP/install-guard.sh"
grep -Fq 'planar.db' "$TMP/install-guard.sh" || fail "install-side ownership guard block not found in install.sh"
run_install_guard() {
  PLANAR_HOME="$1" GUARD="$TMP/install-guard.sh" bash -c '
    set -eEuo pipefail
    FORCE=0
    err() { printf "%s\n" "$*" >&2; exit 1; }
    source "$GUARD"
  '
}
GUARD_DB_PREFIX="$TMP/guard_db/.planar"
mkdir -p "$GUARD_DB_PREFIX"
printf 'main db\n' > "$GUARD_DB_PREFIX/planar.db"
run_install_guard "$GUARD_DB_PREFIX"
# agent.db alone is no longer an ownership signal (plan 1089).
GUARD_AGENT_PREFIX="$TMP/guard_agent/.planar"
mkdir -p "$GUARD_AGENT_PREFIX"
printf 'agent db\n' > "$GUARD_AGENT_PREFIX/agent.db"
if run_install_guard "$GUARD_AGENT_PREFIX" 2>/dev/null; then
  fail "the install-side ownership guard accepted a prefix holding only agent.db"
fi
GUARD_FOREIGN_PREFIX="$TMP/guard_foreign/.planar"
mkdir -p "$GUARD_FOREIGN_PREFIX"
printf 'not ours\n' > "$GUARD_FOREIGN_PREFIX/notes.txt"
if run_install_guard "$GUARD_FOREIGN_PREFIX" 2>"$TMP/guard-foreign-stderr"; then
  echo "expected the install-side ownership guard to refuse a foreign prefix" >&2
  exit 1
fi
grep -Fq 'does not look like a Planar install' "$TMP/guard-foreign-stderr" \
  || fail "install-side ownership guard refused for an unexpected reason"

# File modes (task 7102; tech spec 656 "File modes", replacing decision
# 1210): the installer makes the install root 0700 and tightens an existing
# planar.db (and its sidecars) to 0600, since it holds task claim tokens. The
# retired agent.db is not tightened: install removes it. The function is
# exercised through the exact block extracted from install.sh, like the
# ownership guard above.
mode_of() { stat -f '%Lp' "$1" 2>/dev/null || stat -c '%a' "$1"; }
assert_mode() { [[ "$(mode_of "$1")" == "$2" ]] || fail "expected mode $2 on $1, got $(mode_of "$1")"; }
sed -n '/^harden_planar_home() {/,/^}/p' "$ROOT/install.sh" > "$TMP/harden.sh"
grep -Fq 'chmod' "$TMP/harden.sh" || fail "harden_planar_home not found in install.sh"
run_harden() {
  PLANAR_HOME="$1" HARDEN="$TMP/harden.sh" bash -c '
    set -eEuo pipefail
    vlog() { :; }
    warn() { printf "warn: %s\n" "$*" >&2; }
    source "$HARDEN"
    harden_planar_home
  '
}
MODES_PREFIX="$TMP/modes_home/.planar"
mkdir -p "$MODES_PREFIX/queue-logs"
chmod 755 "$MODES_PREFIX"
for f in planar.db planar.db-wal planar.db-shm agent.db agent.db-wal agent.db-shm; do
  printf 'data\n' > "$MODES_PREFIX/$f"
  chmod 644 "$MODES_PREFIX/$f"
done
printf 'foreign\n' > "$MODES_PREFIX/notes.txt"
chmod 644 "$MODES_PREFIX/notes.txt"
printf 'log\n' > "$MODES_PREFIX/queue-logs/1.log"
chmod 600 "$MODES_PREFIX/queue-logs/1.log"
chmod 700 "$MODES_PREFIX/queue-logs"
run_harden "$MODES_PREFIX" || fail "harden_planar_home failed on an existing install"
assert_mode "$MODES_PREFIX" 700
for f in planar.db planar.db-wal planar.db-shm; do
  assert_mode "$MODES_PREFIX/$f" 600
done
for f in agent.db agent.db-wal agent.db-shm; do
  assert_mode "$MODES_PREFIX/$f" 644
done
assert_mode "$MODES_PREFIX/notes.txt" 644
assert_mode "$MODES_PREFIX/queue-logs" 700
assert_mode "$MODES_PREFIX/queue-logs/1.log" 600

# A prefix with no databases yet is made 0700 and nothing is invented in it.
FRESH_PREFIX="$TMP/fresh_modes/.planar"
mkdir -p "$FRESH_PREFIX"
chmod 755 "$FRESH_PREFIX"
run_harden "$FRESH_PREFIX" || fail "harden_planar_home failed on an empty prefix"
assert_mode "$FRESH_PREFIX" 700
[[ -z "$(ls -A "$FRESH_PREFIX")" ]] || fail "harden_planar_home created files in an empty prefix"

# A database that is a symlink is left alone: its target, outside the prefix,
# keeps its mode.
LINK_PREFIX="$TMP/link_modes/.planar"
LINK_TARGET="$TMP/link_modes/elsewhere.db"
mkdir -p "$LINK_PREFIX"
printf 'data\n' > "$LINK_TARGET"
chmod 644 "$LINK_TARGET"
ln -s "$LINK_TARGET" "$LINK_PREFIX/planar.db"
printf 'data\n' > "$LINK_PREFIX/planar.db-wal"
chmod 644 "$LINK_PREFIX/planar.db-wal"
run_harden "$LINK_PREFIX" || fail "harden_planar_home failed beside a symlinked database"
assert_mode "$LINK_TARGET" 644
assert_mode "$LINK_PREFIX/planar.db-wal" 600

# A chmod that fails only warns, and the function still returns 0: under
# `set -e` and the ERR trap an abort would end the install. A stub chmod
# that always fails stands in for a vanished sidecar or a file not owned by
# the user.
FAIL_PREFIX="$TMP/fail_modes/.planar"
mkdir -p "$FAIL_PREFIX"
printf 'data\n' > "$FAIL_PREFIX/planar.db"
printf 'data\n' > "$FAIL_PREFIX/planar.db-wal"
mkdir -p "$TMP/failbin"
printf '#!/bin/sh\nexit 1\n' > "$TMP/failbin/chmod"
chmod +x "$TMP/failbin/chmod"
PATH="$TMP/failbin:$PATH" run_harden "$FAIL_PREFIX" 2>"$TMP/fail-modes-stderr" \
  || fail "harden_planar_home aborted when chmod failed; it must only warn"
grep -Fq "could not restrict $FAIL_PREFIX to mode 700" "$TMP/fail-modes-stderr" \
  || fail "no warning for the install root when chmod failed"
grep -Fq "could not restrict $FAIL_PREFIX/planar.db to mode 600" "$TMP/fail-modes-stderr" \
  || fail "no warning for planar.db when chmod failed"
grep -Fq "could not restrict $FAIL_PREFIX/planar.db-wal to mode 600" "$TMP/fail-modes-stderr" \
  || fail "no warning for planar.db-wal when chmod failed"

# The installer runs it twice, before the build and after the stamp, and both
# calls must come after the dry-run exit so a dry run changes nothing.
_dry_line="$(grep -n 'Re-run without --dry-run to apply' "$ROOT/install.sh" | head -1 | cut -d: -f1)"
_calls="$(grep -n '^harden_planar_home$' "$ROOT/install.sh" | cut -d: -f1)"
[[ "$(printf '%s\n' "$_calls" | grep -c .)" == 2 ]] \
  || fail "install.sh must call harden_planar_home exactly twice (before the build and after the stamp)"
[[ -n "$_dry_line" ]] || fail "dry-run marker not found in install.sh"
for _call_line in $_calls; do
  [[ "$_call_line" -gt "$_dry_line" ]] \
    || fail "install.sh calls harden_planar_home at line $_call_line, before the dry-run exit (line $_dry_line)"
done

# An uninstall keeps the tightened modes on the files it preserves. (The
# text placeholders standing in for agent.db above are not a store the
# live-queue guard can read, so they go first.)
MODES_HOME="$TMP/modes_home"
rm -f "$MODES_PREFIX/agent.db" "$MODES_PREFIX/agent.db-wal" "$MODES_PREFIX/agent.db-shm"
run_uninstall "$MODES_HOME" >"$TMP/modes-uninstall-stdout" 2>"$TMP/modes-uninstall-stderr" \
  || fail "uninstall of a hardened prefix failed: $(cat "$TMP/modes-uninstall-stderr")"
assert_present "$MODES_PREFIX/planar.db"
assert_mode "$MODES_PREFIX/planar.db" 600
assert_mode "$MODES_PREFIX" 700

# ---------------------------------------------------------------------------
# The queue store probe (plan 1089, task qp-install-migrate; tech spec 656,
# "Install and upgrade", step 3; test spec 658). install.sh sources
# scripts/install-lib/queue-retire.sh after it installs the binaries; these
# cases source the same file and drive queue_probe_migrate with STUB
# planar-agent and planar binaries in a scratch prefix, so the probe table is
# pinned before anything is built. The post-build ctest case
# `install_queue_probe_migrate` drives it with the real binaries.
#
# The stub planar-agent answers its Nth call from $STUB_DIR/agent.N.{code,out}
# and records "cwd|PLANAR_DB|args"; the stub planar records
# "cwd|PLANAR_DB|PLANAR_CONFIG_PATH|args" and answers from
# $STUB_DIR/planar.{code,err}.
QR_LIB="$ROOT/scripts/install-lib/queue-retire.sh"
[[ -f "$QR_LIB" ]] || fail "missing $QR_LIB"
qr_setup() {
  QR_CASE="$TMP/qr_$1"
  QR_P="$QR_CASE/prefix"
  QR_S="$QR_CASE/stub"
  mkdir -p "$QR_P/bin" "$QR_S" "$QR_P/queue-logs"
  cat > "$QR_P/bin/planar-agent" <<'STUB'
#!/usr/bin/env bash
n=$(( $(cat "$STUB_DIR/agent.n" 2>/dev/null || echo 0) + 1 ))
printf '%s\n' "$n" > "$STUB_DIR/agent.n"
printf '%s|%s|%s\n' "$PWD" "${PLANAR_DB-<unset>}" "$*" >> "$STUB_DIR/agent.calls"
[[ -f "$STUB_DIR/agent.$n.out" ]] && cat "$STUB_DIR/agent.$n.out"
exit "$(cat "$STUB_DIR/agent.$n.code" 2>/dev/null || echo 99)"
STUB
  cat > "$QR_P/bin/planar" <<'STUB'
#!/usr/bin/env bash
printf '%s|%s|%s|%s\n' "$PWD" "${PLANAR_DB-<unset>}" "${PLANAR_CONFIG_PATH-<unset>}" "$*" >> "$STUB_DIR/planar.calls"
[[ -f "$STUB_DIR/planar.err" ]] && cat "$STUB_DIR/planar.err" >&2
exit "$(cat "$STUB_DIR/planar.code" 2>/dev/null || echo 0)"
STUB
  chmod +x "$QR_P/bin/planar-agent" "$QR_P/bin/planar"
  printf 'old agent store\n' > "$QR_P/agent.db"
  printf 'old wal\n' > "$QR_P/agent.db-wal"
  printf 'old log\n' > "$QR_P/queue-logs/7.log"
}
# qr_reply N CODE OUT -- the stub planar-agent's Nth answer.
qr_reply() { printf '%s\n' "$2" > "$QR_S/agent.$1.code"; printf '%s' "$3" > "$QR_S/agent.$1.out"; }
qr_json_error() { printf '{"error":{"verb":"queue status","tag":"%s","message":"m"}}\n' "$1"; }
# qr_run FN -- call FN from the seam with the case's prefix; stdout and
# stderr land in $QR_CASE/{out,err}; returns FN's status.
qr_run() {
  (cd "$QR_CASE" && PLANAR_HOME="$QR_P" STUB_DIR="$QR_S" QR_LIB="$QR_LIB" QR_FN="$1" \
    bash -c 'set -eEuo pipefail; source "$QR_LIB"; "$QR_FN"') >"$QR_CASE/out" 2>"$QR_CASE/err"
}
qr_calls() { if [[ -f "$QR_S/$1.calls" ]]; then grep -c . "$QR_S/$1.calls" || true; else echo 0; fi; }
qr_untouched() {
  [[ "$(cat "$QR_P/agent.db")" == "old agent store" ]] || fail "$QR_CASE: agent.db changed"
  [[ "$(cat "$QR_P/agent.db-wal")" == "old wal" ]] || fail "$QR_CASE: agent.db-wal changed"
  [[ "$(cat "$QR_P/queue-logs/7.log")" == "old log" ]] || fail "$QR_CASE: queue-logs/7.log changed"
}

# Empty: no planar.db, so nothing is probed, nothing is migrated, and no
# planar.db appears.
qr_setup none
qr_run queue_probe_migrate || fail "probe step failed on a prefix with no planar.db: $(cat "$QR_CASE/err")"
[[ "$(qr_calls agent)" == 0 && "$(qr_calls planar)" == 0 ]] || fail "a prefix with no planar.db was probed or migrated"
[[ ! -e "$QR_P/planar.db" ]] || fail "the probe step created planar.db"

# Usable: exit 0 with a status object, or exit 1 with tag not_found. One
# read-only probe from /, naming the prefix database, and no init.
for qr_usable in "0|{\"seq\":1,\"state\":\"ended\"}" "1|$(qr_json_error not_found)"; do
  qr_setup "usable_${qr_usable%%|*}"
  printf 'db\n' > "$QR_P/planar.db"
  qr_reply 1 "${qr_usable%%|*}" "${qr_usable#*|}"
  qr_run queue_probe_migrate || fail "a usable probe (${qr_usable%%|*}) failed: $(cat "$QR_CASE/err")"
  [[ "$(cat "$QR_S/agent.calls")" == "/|$QR_P/planar.db|queue status 1 --json" ]] \
    || fail "the probe was not run from / against the prefix planar.db: $(cat "$QR_S/agent.calls")"
  [[ "$(qr_calls planar)" == 0 ]] || fail "a usable planar.db was migrated"
  qr_untouched
done

# Behind: migrate with init from /, naming the prefix database and config,
# then probe again.
qr_setup behind
printf 'db\n' > "$QR_P/planar.db"
qr_reply 1 125 "$(qr_json_error schema_version_behind)"
qr_reply 2 1 "$(qr_json_error not_found)"
qr_run queue_probe_migrate || fail "a behind planar.db was not migrated: $(cat "$QR_CASE/err")"
[[ "$(cat "$QR_S/planar.calls")" == "/|$QR_P/planar.db|$QR_P/config.toml|init --skip-project --allow-no-repo" ]] \
  || fail "the migrate command was not the documented one: $(cat "$QR_S/planar.calls")"
[[ "$(qr_calls agent)" == 2 ]] || fail "a migrated planar.db was not probed again"
[[ ! -e "$QR_P/config.toml" ]] || fail "the migrate step created a config file"

# A migration that fails aborts, prints its stderr and the remedy, and leaves
# agent.db and its logs alone.
qr_setup migrate_fails
printf 'db\n' > "$QR_P/planar.db"
qr_reply 1 125 "$(qr_json_error schema_version_behind)"
printf '1\n' > "$QR_S/planar.code"
printf 'boom' > "$QR_S/planar.err"
if qr_run queue_probe_migrate; then fail "a failed migration did not abort"; fi
grep -Fq 'planar.db migration failed: boom' "$QR_CASE/err" || fail "the migration failure did not name its stderr: $(cat "$QR_CASE/err")"
grep -Fq 'agent.db was NOT retired' "$QR_CASE/err" || fail "the migration failure did not say agent.db was kept"
grep -Fq 'init --skip-project --allow-no-repo' "$QR_CASE/err" || fail "the migration failure did not print the command to run by hand"
[[ "$(qr_calls agent)" == 1 ]] || fail "a failed migration was probed again"
qr_untouched

# A migration after which the database is still not usable aborts.
qr_setup still_behind
printf 'db\n' > "$QR_P/planar.db"
qr_reply 1 125 "$(qr_json_error schema_version_behind)"
qr_reply 2 125 "$(qr_json_error schema_version_behind)"
if qr_run queue_probe_migrate; then fail "a planar.db still behind after init did not abort"; fi
qr_untouched

# Ahead and incompatible, or the same number with a foreign migration: warn,
# naming which, and continue without migrating. An ahead planar.db is never
# refused.
qr_setup incompatible
printf 'db\n' > "$QR_P/planar.db"
qr_reply 1 125 "$(qr_json_error queue_schema_incompatible)"
qr_run queue_probe_migrate || fail "an incompatible ahead planar.db was refused: $(cat "$QR_CASE/err")"
grep -Fq 'ahead of this build' "$QR_CASE/err" || fail "the incompatible warning does not say ahead"
grep -Fq 'until a newer build is installed' "$QR_CASE/err" || fail "the incompatible warning does not name a newer build"
[[ "$(qr_calls planar)" == 0 ]] || fail "an incompatible ahead planar.db was migrated"
qr_setup foreign
printf 'db\n' > "$QR_P/planar.db"
qr_reply 1 125 "$(qr_json_error queue_schema_foreign)"
qr_run queue_probe_migrate || fail "a foreign planar.db was refused: $(cat "$QR_CASE/err")"
grep -Fq 'same number, foreign migration' "$QR_CASE/err" || fail "the foreign warning does not name a foreign migration"
grep -Fq 'a newer build alone will not fix it' "$QR_CASE/err" || fail "the foreign warning does not say a newer build alone will not fix it"
[[ "$(qr_calls planar)" == 0 ]] || fail "a foreign planar.db was migrated"

# Anything else aborts before retire, naming the remedy, with no init and
# agent.db untouched -- including a known tag on the wrong exit code.
qr_fail_case=0
for qr_bad in \
  "125|$(qr_json_error store_unreachable)" \
  "125|$(qr_json_error store_unreadable)" \
  "125|$(qr_json_error internal)" \
  "2|$(qr_json_error invalid_input)" \
  "1|error: status: The following argument was not expected: --json" \
  "1|$(qr_json_error schema_version_behind)" \
  "0|not json" \
  "125|$(qr_json_error not_found)" \
  "7|$(qr_json_error schema_version_ahead)"; do
  qr_fail_case=$((qr_fail_case + 1))
  qr_setup "bad_$qr_fail_case"
  printf 'db\n' > "$QR_P/planar.db"
  qr_reply 1 "${qr_bad%%|*}" "${qr_bad#*|}"
  if qr_run queue_probe_migrate; then fail "probe answer '$qr_bad' did not abort the install"; fi
  grep -Fq 'agent.db was NOT retired' "$QR_CASE/err" || fail "probe answer '$qr_bad' did not say agent.db was kept: $(cat "$QR_CASE/err")"
  grep -Fq 'queue status 1 --json' "$QR_CASE/err" || fail "probe answer '$qr_bad' did not name the remedy"
  [[ "$(qr_calls planar)" == 0 ]] || fail "probe answer '$qr_bad' ran a migration"
  qr_untouched
done

# The call site: install.sh sources the seam, and runs the probe after it
# installs the binaries (the probe IS the newly installed planar-agent), with
# a refusal ending the install.
grep -Fqx 'source "$REPO_ROOT/scripts/install-lib/queue-retire.sh"' "$ROOT/install.sh" \
  || fail "install.sh does not source scripts/install-lib/queue-retire.sh"
_install_line="$(grep -n 'cmake --install "$BUILD_DIR" --prefix "$PLANAR_HOME"' "$ROOT/install.sh" | head -1 | cut -d: -f1)"
_probe_line="$(grep -n '^queue_probe_migrate || exit 1$' "$ROOT/install.sh" | cut -d: -f1)"
[[ -n "$_install_line" ]] || fail "the cmake --install step was not found in install.sh"
[[ "$(printf '%s\n' "$_probe_line" | grep -c .)" == 1 ]] \
  || fail "install.sh must call 'queue_probe_migrate || exit 1' exactly once"
[[ "$_probe_line" -gt "$_install_line" ]] \
  || fail "install.sh probes planar.db (line $_probe_line) before it installs the binaries (line $_install_line)"

# ---------------------------------------------------------------------------
# The live-queue guard and agent.db retirement (plan 1089, task
# qp-install-retire; tech spec 656 steps 1, 4 and 5, "Override", "Uninstall";
# test spec 658). The guard and the retire step are queue_live_guard and
# queue_retire_store in the same sourced file, run here with the real python3
# and queue_retire.py. Rows that must count as live name a sleeping child of
# this test, with host_id and pid_started read by queue_retire.py's OWN
# readers; the post-build ctest case `queue_retire_live_oracle` is what
# checks those readers against the engine.
QR_SLEEPERS=()
qr_kill_sleepers() {
  local pid
  for pid in ${QR_SLEEPERS[@]+"${QR_SLEEPERS[@]}"}; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap 'qr_kill_sleepers; rm -rf "$TMP"' EXIT
# qr_sleeper -- start a sleeping child of this test; its pid lands in QR_PID.
qr_sleeper() {
  sleep 300 &
  QR_PID=$!
  QR_SLEEPERS+=("$QR_PID")
}
# qr_identity PID -- "host_id<TAB>pid_started" as queue_retire.py reads them.
qr_identity() {
  python3 - "$ROOT/scripts/install-lib/queue_retire.py" "$1" <<'PYID'
import importlib.util, sys, time
spec = importlib.util.spec_from_file_location("queue_retire", sys.argv[1])
qr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qr)
pid = int(sys.argv[2])
for _ in range(200):
    started = qr.start_time(pid)
    if started is not None:
        break
    time.sleep(0.01)
print("{}\t{}".format(qr.host_identity(), started))
PYID
}
# qr_live_row_sql SEQ PID -- an insert of a waiting row owned by live PID.
qr_live_row_sql() {
  local ident host started
  ident="$(qr_identity "$2")"
  host="${ident%%	*}"; started="${ident#*	}"
  [[ -n "$host" && "$host" != None && "$started" =~ ^[0-9]+$ ]] || fail "cannot read this host's identity or pid $2's start time: $ident"
  printf "insert into queue_entries (seq, state, host_id, pid, pid_started, cwd, argv, enqueued_at, refreshed_mono) values (%s, 'waiting', '%s', %s, %s, '/', '[\"sleep\",\"300\"]', 1, 1);" \
    "$1" "$host" "$2" "$started"
}
# qr_retire_setup NAME -- a prefix with planar.db and queue-logs/ holding old
# and new logs; the caller builds agent.db (qr_store).
qr_retire_setup() {
  QR_CASE="$TMP/qrr_$1"
  QR_P="$QR_CASE/prefix"
  mkdir -p "$QR_P/queue-logs"
  printf 'main db\n' > "$QR_P/planar.db"
  local n
  for n in 7 12 57 58 1000003; do printf 'log %s\n' "$n" > "$QR_P/queue-logs/$n.log"; done
  printf 'notes\n' > "$QR_P/queue-logs/notes.txt"
}
# qr_store [SQL] -- agent.db for the case, then its -wal and -shm. (The
# sidecars come second: a read-write SQLite connection to a rollback-journal
# database removes a stray -wal.)
qr_store() {
  make_agent_store "$QR_P/agent.db" "${1:-}"
  printf 'old wal\n' > "$QR_P/agent.db-wal"
  printf 'old shm\n' > "$QR_P/agent.db-shm"
}
# qr_call [VAR=VALUE ...] FN [ARG] -- call a seam function for $QR_P, output to
# $QR_CASE/{out,err}.
qr_call() {
  local -a vars=()
  while [[ "$1" == *=* ]]; do vars+=("$1"); shift; done
  (cd "$QR_CASE" && env ${vars[@]+"${vars[@]}"} PLANAR_HOME="$QR_P" QR_LIB="$QR_LIB" \
    bash -c 'set -eEuo pipefail; source "$QR_LIB"; "$@"' qr-call "$@") >"$QR_CASE/out" 2>"$QR_CASE/err"
}
# qr_snapshot -- every file's name and bytes under the prefix, except the
# CONTENT of agent.db-shm: that is SQLite's shared-memory index, which any
# reader of a store with a -wal beside it (this read-only one included) may
# update by protocol. Its presence still counts.
qr_snapshot() {
  (cd "$QR_P" && find . -type f ! -name agent.db-shm -print0 | sort -z | xargs -0 cat | cksum; find . | sort)
}

# Happy path: an idle store (old max 57) is retired. agent.db and its
# sidecars, and the old logs numbered at or below 57 are removed, each named;
# logs above it (58, and a new queue's 1000003) and other files stay.
qr_retire_setup idle
qr_store "insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) values (57, 'exited', '/', '[]', 1, 2, 0); insert into sqlite_sequence (name, seq) values ('queue_entries', 57);"
qr_call queue_live_guard re-check || fail "the guard refused an idle agent.db: $(cat "$QR_CASE/err")"
qr_call queue_retire_store || fail "retire refused an idle agent.db: $(cat "$QR_CASE/err")"
for f in agent.db agent.db-wal agent.db-shm queue-logs/7.log queue-logs/12.log queue-logs/57.log; do
  [[ ! -e "$QR_P/$f" ]] || fail "retire left $f"
  grep -Fq "$QR_P/$f" "$QR_CASE/out" || fail "retire did not name the removal of $f: $(cat "$QR_CASE/out")"
done
for f in planar.db queue-logs/58.log queue-logs/1000003.log queue-logs/notes.txt; do
  [[ -e "$QR_P/$f" ]] || fail "retire removed $f"
done
run_harden "$QR_P" || fail "harden_planar_home failed after retire"
assert_mode "$QR_P" 700
assert_mode "$QR_P/planar.db" 600

# The floor: an old store whose range reaches 1,000,000 cannot be told apart
# from the new queue's by number, so retire refuses, naming both numbers,
# and removes nothing -- --ignore-live-queue does not change that.
for qr_floor in 1000000 1234567; do
  qr_retire_setup "floor_$qr_floor"
  qr_store "insert into sqlite_sequence (name, seq) values ('queue_entries', $qr_floor);"
  qr_before="$(qr_snapshot)"
  for qr_ignore in 0 1; do
    if qr_call IGNORE_LIVE_QUEUE=$qr_ignore queue_retire_store; then
      fail "retire accepted an old range reaching the floor ($qr_floor, ignore=$qr_ignore)"
    fi
    grep -Fq "is $qr_floor" "$QR_CASE/err" || fail "the floor refusal does not name the old maximum $qr_floor"
    grep -Fq "floor 1000000" "$QR_CASE/err" || fail "the floor refusal does not name the floor"
    [[ "$(qr_snapshot)" == "$qr_before" ]] || fail "a refused retire changed the prefix"
  done
done

# A live old entry blocks, naming the seq, the pid and the remedies;
# --ignore-live-queue turns that into a warning naming the cost, and then
# retires. (--force is not an override; see the install.sh cases below.)
qr_sleeper
QR_LIVE_PID="$QR_PID"
qr_retire_setup live
qr_store "$(qr_live_row_sql 3 "$QR_LIVE_PID")"
qr_before="$(qr_snapshot)"
if qr_call queue_live_guard preflight; then fail "the guard passed a live old entry"; fi
for phrase in "seq=3 " "pid=$QR_LIVE_PID " "planar-agent queue cancel <seq>" "--ignore-live-queue" "--force does not bypass"; do
  grep -Fq -- "$phrase" "$QR_CASE/err" || fail "the live-entry refusal does not name '$phrase': $(cat "$QR_CASE/err")"
done
[[ "$(qr_snapshot)" == "$qr_before" ]] || fail "a refused guard changed the prefix"
qr_call IGNORE_LIVE_QUEUE=1 queue_live_guard re-check || fail "--ignore-live-queue did not override a live entry"
grep -Fq 'orphaned queue' "$QR_CASE/err" || fail "the override did not print its cost"
qr_call IGNORE_LIVE_QUEUE=1 queue_retire_store || fail "retire refused after the override: $(cat "$QR_CASE/err")"
[[ ! -e "$QR_P/agent.db" ]] || fail "agent.db survived an overridden retire"

# The installer's own preflight: the same live entry refuses before anything
# is built, --force refuses the same way, and --ignore-live-queue proceeds
# with the warning. Always a dry run against a scratch HOME and prefix, so
# nothing is ever built or installed, whatever the guard does.
qr_retire_setup install_live
qr_store "$(qr_live_row_sql 4 "$QR_LIVE_PID")"
qr_install() {
  HOME="$QR_CASE/home" PLANAR_HOME="$QR_P" CODEX_HOME="$QR_CASE/home/.codex" \
    "$ROOT/install.sh" --dry-run --no-vendor --build-dir "$QR_CASE/build" "$@" >"$QR_CASE/out" 2>"$QR_CASE/err"
}
mkdir -p "$QR_CASE/home"
for qr_flags in "" "--force"; do
  if qr_install $qr_flags; then fail "install.sh $qr_flags passed a live old entry"; fi
  grep -Fq 'seq=4 ' "$QR_CASE/err" || fail "install.sh $qr_flags did not refuse for the live entry: $(cat "$QR_CASE/err")"
  ! grep -Fq 'missing required' "$QR_CASE/err" || fail "install.sh $qr_flags ran the dependency preflight before the live-queue preflight"
  ! grep -Fq 'Dry run' "$QR_CASE/out" || fail "install.sh $qr_flags got past the live-queue preflight"
done
qr_install --ignore-live-queue || true
grep -Fq 'orphaned queue' "$QR_CASE/err" || fail "install.sh --ignore-live-queue did not warn with the cost: $(cat "$QR_CASE/err")"
! grep -Fq 'refusing to retire' "$QR_CASE/err" || fail "install.sh --ignore-live-queue still refused"
[[ -e "$QR_P/agent.db" ]] || fail "a dry run removed agent.db"

# Only provably dead rows (a reaped submitter on this host; a row from an
# earlier boot of this machine or another machine) do not block: the guard
# passes without the override, lists them as ignored, and retire proceeds.
qr_retire_setup dead
true & qr_dead_pid=$!; wait "$qr_dead_pid" || true
qr_ident="$(qr_identity "$QR_LIVE_PID")"
qr_host="${qr_ident%%	*}"
qr_rest=""; [[ "$qr_host" == *:* ]] && qr_rest=":${qr_host#*:}"
qr_store "insert into queue_entries (seq, state, host_id, pid, pid_started, cwd, argv, enqueued_at, refreshed_mono) values (5, 'waiting', '$qr_host', $qr_dead_pid, 1, '/', '[]', 1, 1), (6, 'waiting', '00000000-0000-0000-0000-000000000000$qr_rest', $QR_LIVE_PID, 1, '/', '[]', 1, 1);"
qr_call queue_live_guard re-check || fail "the guard refused only dead rows: $(cat "$QR_CASE/err")"
grep -Fq 'ignoring dead seq=5 ' "$QR_CASE/out" || fail "the guard did not list the reaped row as ignored: $(cat "$QR_CASE/out")"
grep -Fq 'ignoring dead seq=6 ' "$QR_CASE/out" || fail "the guard did not list the earlier-boot row as ignored"
qr_call queue_retire_store || fail "retire refused after only dead rows: $(cat "$QR_CASE/err")"

# The re-check: the preflight passes on an empty store, an entry appears
# (as it could during the build), and the re-check refuses before removal.
qr_retire_setup recheck
qr_store
qr_call queue_live_guard preflight || fail "the preflight refused an empty store"
python3 -c 'import sqlite3, sys; c = sqlite3.connect(sys.argv[1]); c.executescript(sys.argv[2]); c.close()' \
  "$QR_P/agent.db" "$(qr_live_row_sql 9 "$QR_LIVE_PID")"
if qr_call queue_live_guard re-check; then fail "the re-check missed an entry that appeared after the preflight"; fi
grep -Fq 'seq=9 ' "$QR_CASE/err" || fail "the re-check did not name the new entry"
[[ -e "$QR_P/agent.db" ]] || fail "a refused re-check removed agent.db"

# A store that cannot be read is live: the guard refuses, naming the manual
# remedy. --ignore-live-queue makes the guard warn, but retire still refuses
# an unreadable store, and nothing is touched.
qr_bad_store=0
for qr_kind in text no_table no_sequence mode000; do
  if [[ "$qr_kind" == mode000 && "$(id -u)" == 0 ]]; then continue; fi
  qr_bad_store=$((qr_bad_store + 1))
  qr_retire_setup "unreadable_$qr_kind"
  case "$qr_kind" in
    text) printf 'not a database\n' > "$QR_P/agent.db" ;;
    no_table) python3 -c 'import sqlite3, sys; c = sqlite3.connect(sys.argv[1]); c.execute("create table other (x)"); c.close()' "$QR_P/agent.db" ;;
    no_sequence) qr_store "insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) values (3, 'exited', '/', '[]', 1, 2, 0);" ;;
    mode000) qr_store; chmod 000 "$QR_P/agent.db" ;;
  esac
  qr_before="$(cd "$QR_P" && ls -lR | grep -v -e 'agent.db-shm' -e '^total ' | cksum)"
  if [[ "$qr_kind" != no_sequence ]]; then
    if qr_call queue_live_guard preflight; then fail "the guard passed an unreadable store ($qr_kind)"; fi
    grep -Fq 'by hand' "$QR_CASE/err" || fail "the unreadable-store refusal ($qr_kind) does not name the manual remedy: $(cat "$QR_CASE/err")"
    qr_call IGNORE_LIVE_QUEUE=1 queue_live_guard re-check || fail "--ignore-live-queue did not turn an unreadable store ($qr_kind) into a warning"
  fi
  if qr_call IGNORE_LIVE_QUEUE=1 queue_retire_store; then fail "retire accepted an unreadable store ($qr_kind)"; fi
  grep -Fq 'NOT retired' "$QR_CASE/err" || fail "the retire refusal ($qr_kind) does not say agent.db was kept"
  [[ "$(cd "$QR_P" && ls -lR | grep -v -e 'agent.db-shm' -e '^total ' | cksum)" == "$qr_before" ]] || fail "a refused retire ($qr_kind) changed the prefix"
  chmod 600 "$QR_P/agent.db" 2>/dev/null || true
done
[[ "$qr_bad_store" -ge 3 ]] || fail "too few unreadable-store cases ran"

# No agent.db: neither the guard nor retire runs, and python3 is not invoked.
qr_retire_setup none
rm -f "$QR_P/agent.db-wal" "$QR_P/agent.db-shm"
mkdir -p "$QR_CASE/spybin"
printf '#!/bin/sh\necho called >> "%s/python3.calls"\nexit 99\n' "$QR_CASE" > "$QR_CASE/spybin/python3"
chmod +x "$QR_CASE/spybin/python3"
qr_before="$(qr_snapshot)"
qr_call PATH="$QR_CASE/spybin:$PATH" queue_live_guard preflight || fail "the guard failed with no agent.db"
qr_call PATH="$QR_CASE/spybin:$PATH" queue_retire_store || fail "retire failed with no agent.db"
[[ ! -e "$QR_CASE/python3.calls" ]] || fail "python3 was invoked with no agent.db"
[[ "$(qr_snapshot)" == "$qr_before" ]] || fail "the queue steps changed a prefix with no agent.db"

# The call site: the queue-store block of install.sh runs the probe, the
# re-check and retire, in that order, and a refused re-check stops it before
# retire. The block is extracted and run with stub functions.
sed -n '/^# ---------- queue store ----------$/,/^# ---------- place artifacts ----------$/p' "$ROOT/install.sh" > "$TMP/queue-block.sh"
grep -Fq 'queue_retire_store' "$TMP/queue-block.sh" || fail "the queue store block was not found in install.sh"
run_queue_block() {
  QUEUE_BLOCK="$TMP/queue-block.sh" RECHECK_RC="$1" bash -c '
    set -eEuo pipefail
    title() { :; }
    queue_probe_migrate() { echo probe; }
    queue_live_guard() { echo "guard $1"; return "$RECHECK_RC"; }
    queue_retire_store() { echo retire; }
    source "$QUEUE_BLOCK"
  '
}
[[ "$(run_queue_block 0 | tr '\n' ' ')" == "probe guard re-check retire " ]] \
  || fail "install.sh's queue store block does not run probe, re-check, retire in order: $(run_queue_block 0 | tr '\n' ' ')"
if qr_block_out="$(run_queue_block 1)"; then fail "a refused re-check did not stop install.sh"; fi
[[ "$qr_block_out" != *retire* ]] || fail "install.sh retired agent.db after a refused re-check"
# The preflight comes before the dependency preflight and the build.
_pre_line="$(grep -n '^queue_live_guard preflight || exit 1$' "$ROOT/install.sh" | cut -d: -f1)"
_deps_line="$(grep -n '^check_deps "build"' "$ROOT/install.sh" | cut -d: -f1)"
[[ -n "$_pre_line" && -n "$_deps_line" && "$_pre_line" -lt "$_deps_line" ]] \
  || fail "install.sh's live-queue preflight must run before check_deps (lines '$_pre_line' / '$_deps_line')"

# Uninstall: with a live agent.db entry, --uninstall and --uninstall --force
# both refuse before removing anything; --ignore-live-queue proceeds.
for qr_flags in "" "--force"; do
  QR_UHOME="$TMP/qr_uninstall_live${qr_flags}"
  seed_prefix "$QR_UHOME/.planar"
  mkdir -p "$QR_UHOME/.planar/bin"
  printf '#!/bin/sh\n' > "$QR_UHOME/.planar/bin/planar"; chmod +x "$QR_UHOME/.planar/bin/planar"
  rm -f "$QR_UHOME/.planar/agent.db"
  make_agent_store "$QR_UHOME/.planar/agent.db" "$(qr_live_row_sql 11 "$QR_LIVE_PID")"
  if run_uninstall "$QR_UHOME" $qr_flags >"$TMP/qr-u-out" 2>"$TMP/qr-u-err"; then
    fail "uninstall $qr_flags passed a live old entry"
  fi
  grep -Fq 'seq=11 ' "$TMP/qr-u-err" || fail "uninstall $qr_flags refused for an unexpected reason: $(cat "$TMP/qr-u-err")"
  assert_present "$QR_UHOME/.planar/bin/planar"
  assert_present "$QR_UHOME/.planar/agent.db"
  run_uninstall "$QR_UHOME" $qr_flags --ignore-live-queue >"$TMP/qr-u-out" 2>"$TMP/qr-u-err" \
    || fail "uninstall $qr_flags --ignore-live-queue refused: $(cat "$TMP/qr-u-err")"
  assert_absent "$QR_UHOME/.planar/agent.db"
done

# Uninstall without python3: with agent.db present it refuses before removing
# anything, naming python3 and the remedies; --ignore-live-queue proceeds
# with a warning; with no agent.db, python3 is not needed. PATH is a farm of
# the tools the uninstall branch uses, without python3.
QR_NOPY="$TMP/nopy-bin"
mkdir -p "$QR_NOPY"
for tool in bash env dirname find readlink rm rmdir cat realpath ls mkdir grep tr; do
  ln -s "$(command -v "$tool")" "$QR_NOPY/$tool"
done
run_uninstall_nopy() { PATH="$QR_NOPY" run_uninstall "$@"; }
QR_UHOME="$TMP/qr_uninstall_nopy"
seed_prefix "$QR_UHOME/.planar"
if run_uninstall_nopy "$QR_UHOME" >"$TMP/qr-u-out" 2>"$TMP/qr-u-err"; then
  fail "uninstall with agent.db and no python3 did not refuse"
fi
grep -Fq 'python3 is required' "$TMP/qr-u-err" || fail "the no-python3 refusal does not name python3: $(cat "$TMP/qr-u-err")"
grep -Fq -- '--ignore-live-queue' "$TMP/qr-u-err" || fail "the no-python3 refusal does not name --ignore-live-queue"
assert_present "$QR_UHOME/.planar/agent.db"
assert_present "$QR_UHOME/.planar/queue-logs/1000001.log"
run_uninstall_nopy "$QR_UHOME" --ignore-live-queue >"$TMP/qr-u-out" 2>"$TMP/qr-u-err" \
  || fail "uninstall --ignore-live-queue without python3 refused: $(cat "$TMP/qr-u-err")"
grep -Fq 'python3 is not installed' "$TMP/qr-u-err" || fail "uninstall --ignore-live-queue without python3 did not warn"
assert_absent "$QR_UHOME/.planar/agent.db"
QR_UHOME="$TMP/qr_uninstall_nopy_noagent"
seed_prefix "$QR_UHOME/.planar"
rm -f "$QR_UHOME/.planar/agent.db" "$QR_UHOME/.planar/agent.db-wal" "$QR_UHOME/.planar/agent.db-shm"
run_uninstall_nopy "$QR_UHOME" >"$TMP/qr-u-out" 2>"$TMP/qr-u-err" \
  || fail "uninstall with no agent.db needed python3: $(cat "$TMP/qr-u-err")"
assert_present "$QR_UHOME/.planar/planar.db"

# Dependencies: python3's reason names the retirement reader; neither ps nor
# sysctl is a manifest entry or a README prerequisite (question 1011).
grep -Fq '"python3|python|Centurion'"'"'s configure; install.sh'"'"'s agent.db retirement reader' "$ROOT/install.sh" \
  || fail "python3's BUILD_DEPS reason does not name the agent.db retirement reader"
! grep -Eq '^  "(ps|sysctl)\|' "$ROOT/install.sh" || fail "ps or sysctl is in BUILD_DEPS/RUN_DEPS"
sed -n '/^## Prerequisites/,/^### /p' "$ROOT/README.md" > "$TMP/prereqs.md"
grep -Fq 'agent.db` retirement reader' "$TMP/prereqs.md" || fail "README Prerequisites does not name the retirement reader"
! grep -Eq '`(ps|sysctl)`' "$TMP/prereqs.md" || fail "README Prerequisites names ps or sysctl"

qr_kill_sleepers
printf 'install-manifest tests: 12 passed\n'

