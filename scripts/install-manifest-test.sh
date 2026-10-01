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

# Uninstall preserves the operator's data (task hq-install-preserve, plan
# 1080). The agent database `agent.db` (decision "Agent state gets its own
# database"), its SQLite sidecars `agent.db-wal` / `agent.db-shm`, and the
# detached-run output directory `queue-logs/` survive a non-force uninstall
# exactly as `planar.db` does, while every managed artifact is removed. The
# uninstall branch returns before the build-dependency preflight, so this
# drives the real `install.sh --uninstall` against a scratch home.
# /bin/bash 3.2 does not honor `set -e` for a failing bare `[[ … ]]`, so
# every assertion below goes through fail() to be non-vacuous.
fail() { printf 'install-manifest-test: %s\n' "$*" >&2; exit 1; }
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
  printf 'agent db\n' > "$prefix/agent.db"
  printf 'wal\n' > "$prefix/agent.db-wal"
  printf 'shm\n' > "$prefix/agent.db-shm"
  printf 'detached output\n' > "$prefix/queue-logs/1.log"
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
assert_present "$UNINSTALL_PREFIX/planar.db"
assert_present "$UNINSTALL_PREFIX/planar.db-wal"
assert_present "$UNINSTALL_PREFIX/planar.db-shm"
assert_present "$UNINSTALL_PREFIX/agent.db"
assert_present "$UNINSTALL_PREFIX/agent.db-wal"
assert_present "$UNINSTALL_PREFIX/agent.db-shm"
assert_present "$UNINSTALL_PREFIX/queue-logs/1.log"
assert_absent "$UNINSTALL_PREFIX/bin"
assert_absent "$UNINSTALL_PREFIX/skills"
assert_absent "$UNINSTALL_PREFIX/install-manifest.json"
grep -Fq 'agent.db' "$TMP/uninstall-stdout" || fail "uninstall output does not mention agent.db"

# A prefix whose only ownership signal is agent.db (planar.db and the binaries
# already gone) is still a Planar install: the uninstall-side ownership guard
# accepts it without --force, removes the stray entry, and keeps the data.
AGENT_ONLY_HOME="$TMP/agent_only_home"
AGENT_ONLY_PREFIX="$AGENT_ONLY_HOME/.planar"
mkdir -p "$AGENT_ONLY_PREFIX/queue-logs" "$AGENT_ONLY_PREFIX/stale"
printf 'agent db\n' > "$AGENT_ONLY_PREFIX/agent.db"
printf 'detached output\n' > "$AGENT_ONLY_PREFIX/queue-logs/2.log"
run_uninstall "$AGENT_ONLY_HOME" >"$TMP/agent-only-stdout" 2>"$TMP/agent-only-stderr" \
  || fail "uninstall refused a prefix holding only agent.db: $(cat "$TMP/agent-only-stderr")"
assert_present "$AGENT_ONLY_PREFIX/agent.db"
assert_present "$AGENT_ONLY_PREFIX/queue-logs/2.log"
assert_absent "$AGENT_ONLY_PREFIX/stale"

# --force removes the agent database and its logs along with planar.db.
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
GUARD_AGENT_PREFIX="$TMP/guard_agent/.planar"
mkdir -p "$GUARD_AGENT_PREFIX"
printf 'agent db\n' > "$GUARD_AGENT_PREFIX/agent.db"
run_install_guard "$GUARD_AGENT_PREFIX"
GUARD_FOREIGN_PREFIX="$TMP/guard_foreign/.planar"
mkdir -p "$GUARD_FOREIGN_PREFIX"
printf 'not ours\n' > "$GUARD_FOREIGN_PREFIX/notes.txt"
if run_install_guard "$GUARD_FOREIGN_PREFIX" 2>"$TMP/guard-foreign-stderr"; then
  echo "expected the install-side ownership guard to refuse a foreign prefix" >&2
  exit 1
fi
grep -Fq 'does not look like a Planar install' "$TMP/guard-foreign-stderr" \
  || fail "install-side ownership guard refused for an unexpected reason"

# Decision 1210 (task 7102): the installer makes the install root 0700 and
# tightens an existing planar.db / agent.db (and their sidecars) to 0600,
# since both hold task claim tokens. The function is exercised through the
# exact block extracted from install.sh, like the ownership guard above.
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
for f in planar.db planar.db-wal planar.db-shm agent.db agent.db-wal agent.db-shm; do
  assert_mode "$MODES_PREFIX/$f" 600
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
printf 'data\n' > "$LINK_PREFIX/agent.db"
chmod 644 "$LINK_PREFIX/agent.db"
run_harden "$LINK_PREFIX" || fail "harden_planar_home failed beside a symlinked database"
assert_mode "$LINK_TARGET" 644
assert_mode "$LINK_PREFIX/agent.db" 600

# A chmod that fails only warns, and the function still returns 0: under
# `set -e` and the ERR trap an abort would end the install. A stub chmod
# that always fails stands in for a vanished sidecar or a file not owned by
# the user.
FAIL_PREFIX="$TMP/fail_modes/.planar"
mkdir -p "$FAIL_PREFIX"
printf 'data\n' > "$FAIL_PREFIX/planar.db"
printf 'data\n' > "$FAIL_PREFIX/agent.db-wal"
mkdir -p "$TMP/failbin"
printf '#!/bin/sh\nexit 1\n' > "$TMP/failbin/chmod"
chmod +x "$TMP/failbin/chmod"
PATH="$TMP/failbin:$PATH" run_harden "$FAIL_PREFIX" 2>"$TMP/fail-modes-stderr" \
  || fail "harden_planar_home aborted when chmod failed; it must only warn"
grep -Fq "could not restrict $FAIL_PREFIX to mode 700" "$TMP/fail-modes-stderr" \
  || fail "no warning for the install root when chmod failed"
grep -Fq "could not restrict $FAIL_PREFIX/planar.db to mode 600" "$TMP/fail-modes-stderr" \
  || fail "no warning for planar.db when chmod failed"
grep -Fq "could not restrict $FAIL_PREFIX/agent.db-wal to mode 600" "$TMP/fail-modes-stderr" \
  || fail "no warning for agent.db-wal when chmod failed"

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

# An uninstall keeps the tightened modes on the files it preserves.
MODES_HOME="$TMP/modes_home"
run_uninstall "$MODES_HOME" >"$TMP/modes-uninstall-stdout" 2>"$TMP/modes-uninstall-stderr" \
  || fail "uninstall of a hardened prefix failed: $(cat "$TMP/modes-uninstall-stderr")"
assert_present "$MODES_PREFIX/agent.db"
assert_mode "$MODES_PREFIX/agent.db" 600
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

printf 'install-manifest tests: 11 passed\n'
