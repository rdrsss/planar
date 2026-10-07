#!/bin/sh
# release-smoke.sh -- the clean-host smoke gate for one extracted release bundle.
#
# Usage: release-smoke.sh <bundle-dir>
#
# Runs the bundle's own binaries with no developer toolchain: scripts/release-gates.sh
# starts it under `env -i` with a system-only PATH on macOS, and inside a bare
# debian:bookworm-slim container (no LLVM, no Python) for Linux. POSIX sh and base
# utilities only, because the Linux container has nothing else. Every check uses a
# scratch HOME, PLANAR_DB and PLANAR_CONFIG_PATH created here and removed on exit.
#
# Output contract, read by release-gates.sh: one `smoke_expected=<n>` line, one
# `smoke_check=<name> result=pass|fail` line per check, then `smoke_passed=<n>`.
# Exits 0 only when every expected check passed.
set -u

[ "$#" = 1 ] || { echo "usage: release-smoke.sh <bundle-dir>" >&2; exit 2; }
bundle=$1
release="$bundle/release.json"
[ -f "$release" ] || { echo "release-smoke: missing $release" >&2; exit 1; }

# release.json is flat, one key per line (scripts/dist.sh writes it so).
field() { sed -n "s/^  \"$1\": \\(.*\\)\$/\\1/p" "$release" | sed 's/,$//'; }
version=$(field version | tr -d '"')
sha=$(field sha | tr -d '"')
schema=$(field schema_version)
[ -n "$version" ] && [ -n "$sha" ] && [ -n "$schema" ] ||
  { echo "release-smoke: $release lacks version, sha or schema_version" >&2; exit 1; }
version_re=$(printf '%s' "$version" | sed 's/\./\\./g')

arena=$(mktemp -d "${TMPDIR:-/tmp}/planar-smoke.XXXXXX") || exit 1
trap 'rm -rf "$arena"' EXIT
HOME="$arena/home"
PLANAR_DB="$arena/planar.db"
PLANAR_CONFIG_PATH="$arena/config.toml"
export HOME PLANAR_DB PLANAR_CONFIG_PATH
unset GIT_DIR GIT_WORK_TREE PLANAR_HOME
mkdir -p "$HOME" || exit 1
out="$arena/out"

expected=7
passed=0
echo "smoke_expected=$expected"
record() { # record NAME STATUS
  if [ "$2" = 0 ]; then
    passed=$((passed + 1))
    echo "smoke_check=$1 result=pass"
  else
    echo "smoke_check=$1 result=fail"
    sed 's/^/  /' "$out"
  fi
}

# The four version-bearing binaries report the bundle's own release and commit.
for name in planar planar-agent planar-watch planar-ext; do
  rc=0
  "$bundle/bin/$name" version --json > "$out" 2>&1 || rc=$?
  if [ "$rc" = 0 ]; then
    grep -q "\"release\": *\"$version_re\"" "$out" && grep -q "\"sha\": *\"$sha\"" "$out" || rc=1
  fi
  record "version-$name" "$rc"
done

rc=0
"$bundle/bin/planar-execute" --help > "$out" 2>&1 || rc=$?
record help-planar-execute "$rc"

rc=0
(cd "$HOME" && "$bundle/bin/planar" init --skip-project --allow-no-repo) > "$out" 2>&1 || rc=$?
record init "$rc"

# A fresh database migrates to exactly the schema the bundle declares.
rc=0
(cd "$HOME" && "$bundle/bin/planar" health --json) > "$out" 2>&1 || rc=$?
if [ "$rc" = 0 ]; then
  grep -q '"schema_current": *true' "$out" && grep -q "\"schema_target\": *${schema}[,}]" "$out" || rc=1
fi
record health "$rc"

echo "smoke_passed=$passed"
[ "$passed" = "$expected" ]
