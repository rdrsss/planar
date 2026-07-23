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
# legacy in-band scheme retires with skillrender; scriptorium tracks install
# drift out-of-band in its own merkle+xxhash manifest — tech-spec.md D5).
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

printf 'install-manifest tests: 3 passed\n'
