#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Run an isolated installer copy whose pinned C++ compiler path is unavailable.
# The dependency preflight must refuse before it can invoke CMake.
mkdir -p "$TMP/repo/scripts" "$TMP/repo/skills/planar" "$TMP/repo/agents" "$TMP/repo/workflows" "$TMP/repo/migrations" "$TMP/home"
cp "$ROOT/install.sh" "$TMP/repo/install.sh"
cp -R "$ROOT/scripts/install-lib" "$TMP/repo/scripts/"
cp "$ROOT/scripts/uninstall.sh" "$TMP/repo/scripts/"

perl -0pi -e 's#/opt/homebrew/opt/llvm/bin/clang\+\+#/definitely/missing/planar-clang++#g' \
  "$TMP/repo/install.sh"

# A scratch HOME and prefix: even a dry run inspects the prefix (plan 1089's
# queue steps look for planar.db there), and this test must never look at the
# operator's ~/.planar.
if HOME="$TMP/home" PLANAR_HOME="$TMP/home/.planar" CODEX_HOME="$TMP/home/.codex" \
  "$TMP/repo/install.sh" --dry-run --no-vendor >"$TMP/stdout" 2>"$TMP/stderr"; then
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

# The prebuilt path checks the base tier only: with the pinned compiler missing
# (as above) a prebuilt dry run still passes; with realpath missing it refuses,
# naming the tool, before it creates anything.
# shellcheck source=fixtures/prebuilt-bundle.sh
source "$ROOT/scripts/fixtures/prebuilt-bundle.sh"
fake_bundle_make "$ROOT" "$TMP/bundle"
base_names="$(sed -n '/^BASE_DEPS=(/,/^)/p' "$ROOT/install.sh" | sed -n 's/^  "\([^|"]*\)|.*/\1/p')"
[[ " $(printf '%s' "$base_names" | tr '\n' ' ') " == *" realpath "* ]] || { echo "realpath is not in the base tier" >&2; exit 1; }
mkdir -p "$TMP/basebin" "$TMP/basebin-no-realpath" "$TMP/home2"
for n in $base_names; do
  found="$(command -v "$n")"
  ln -s "$found" "$TMP/basebin/$n"
  [[ "$n" == realpath ]] || ln -s "$found" "$TMP/basebin-no-realpath/$n"
done
if ! HOME="$TMP/home2" PATH="$TMP/basebin" /bin/bash "$TMP/repo/install.sh" --prebuilt "$TMP/bundle" --dry-run --no-vendor \
    --prefix "$TMP/home2/.planar" >"$TMP/pb-stdout" 2>"$TMP/pb-stderr"; then
  echo "a prebuilt dry run was refused although only the toolchain tier is missing" >&2
  cat "$TMP/pb-stderr" >&2
  exit 1
fi
grep -Fq 'prebuilt' "$TMP/pb-stdout"
if grep -Fq 'missing required' "$TMP/pb-stderr"; then echo 'the prebuilt dry run reported a missing tool' >&2; exit 1; fi
if HOME="$TMP/home2" PATH="$TMP/basebin-no-realpath" /bin/bash "$TMP/bundle/install.sh" --prebuilt "$TMP/bundle" --dry-run --no-vendor \
    --prefix "$TMP/home2/.planar" >"$TMP/pb2-stdout" 2>"$TMP/pb2-stderr"; then
  echo "a prebuilt install without realpath was not refused" >&2
  exit 1
fi
grep -Fq 'missing required base tool(s)' "$TMP/pb2-stderr"
grep -Fq 'realpath' "$TMP/pb2-stderr"
[[ ! -e "$TMP/home2/.planar" ]]

printf 'install dependency tests: 4 passed\n'
