#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Run an isolated installer copy whose pinned C++ compiler path is unavailable.
# The dependency preflight must refuse before it can invoke CMake.
mkdir -p "$TMP/repo/scripts"
cp "$ROOT/install.sh" "$TMP/repo/install.sh"
cp "$ROOT/scripts/install-manifest.sh" "$TMP/repo/scripts/"

perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang\+\+#/definitely/missing/planar-clang++#g' \
  "$TMP/repo/install.sh"

if "$TMP/repo/install.sh" --dry-run --no-vendor >"$TMP/stdout" 2>"$TMP/stderr"; then
  echo "expected missing pinned C++ compiler to abort install.sh" >&2
  exit 1
fi

grep -Fq 'missing required build tool(s)' "$TMP/stderr"
grep -Fq '/definitely/missing/planar-clang++' "$TMP/stderr"
! grep -Fq 'Building the Planar binaries' "$TMP/stdout"

# An optional gh token lookup may fail on an unauthenticated machine. The
# installer's inherited ERR trap must stay quiet because the cached first-
# party source can still be used. Exercise the exact sourced block.
mkdir -p "$TMP/fake-bin"
cat > "$TMP/fake-bin/gh" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x "$TMP/fake-bin/gh"
sed -n '/^if \[\[ -z "${GITHUB_TOKEN:-}" \]\] && command -v gh/,/^fi$/p' \
  "$ROOT/install.sh" > "$TMP/token-probe.sh"
PATH="$TMP/fake-bin:$PATH" TOKEN_PROBE="$TMP/token-probe.sh" bash -c '
  set -eEuo pipefail
  unset GITHUB_TOKEN
  on_err() { echo "unexpected ERR trap" >&2; exit 99; }
  trap '\''on_err $? $LINENO'\'' ERR
  source "$TOKEN_PROBE"
' >"$TMP/token-stdout" 2>"$TMP/token-stderr"
[[ ! -s "$TMP/token-stderr" ]]

printf 'install dependency tests: 2 passed\n'
