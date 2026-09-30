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

printf 'install-manifest tests: 10 passed\n'
