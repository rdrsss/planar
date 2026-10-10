#!/usr/bin/env bash
# install-centuriond.sh — put a stock `centuriond` under a Planar prefix
# (plan 1033 M1, task 6709; tech-spec D8 and D13).
#
# Planar never links centuriond. It installs the stock daemon, built from the
# SAME pinned Centurion archive whose client planar-execute links, so the two
# ends of the gRPC socket come from one tag. Two ways to get the binary:
#
#   1. a Centurion RELEASE binary for this platform, when the pinned tag
#      publishes one (`centuriond-<tag>-<os>-<arch>.tar.gz` plus a `.sha256`
#      sibling) — preferred, and verified against that checksum before use;
#   2. otherwise a SOURCE BUILD of the pinned tree as a separate CMake project
#      (daemon on; CLI, and with it the FTXUI terminal UI, off; tests off),
#      under the same pinned LLVM toolchain as Planar.
#
# Either way it installs:
#
#   <prefix>/bin/centuriond
#   <prefix>/share/centurion/migrations/*.{up,down}.sql
#   <prefix>/share/centurion/build-identity.json
#
# The build identity is one element of the client's compatibility tuple
# (tech-spec D7), which "does not care which" path produced the binary (D8)
# — so the identity records both the pin and how it was satisfied.
#
# A source build compiles <prefix>/share/centurion/migrations in as the
# daemon's default migration directory, so the installed daemon needs nothing
# from the source tree at run time. A release binary's default points wherever
# Centurion built it; the identity's `migrations_dir` is what a client passes
# as `--migrations` either way.
#
# Usage:
#   install-centuriond.sh --pin <centurion-pin.env> --prefix <dir>
#                         --build-dir <dir> --toolchain <llvm-toolchain.cmake>
#                         [--source <dir>] [--no-release]
#
#   --pin        the file cmake/centurion.cmake writes into Planar's build tree
#   --source     build from this tree instead of the pin's CENTURION_SOURCE_DIR
#                (the distribution test uses a throwaway copy it later deletes)
#   --no-release skip the release-binary lookup; always build from source

set -euo pipefail

die() { printf 'install-centuriond: %s\n' "$*" >&2; exit 1; }
say() { printf 'install-centuriond: %s\n' "$*"; }

pin="" prefix="" build_dir="" toolchain="" source_override="" try_release=1
while (($#)); do
  case "$1" in
    --pin) pin="${2:?}"; shift 2 ;;
    --prefix) prefix="${2:?}"; shift 2 ;;
    --build-dir) build_dir="${2:?}"; shift 2 ;;
    --toolchain) toolchain="${2:?}"; shift 2 ;;
    --source) source_override="${2:?}"; shift 2 ;;
    --no-release) try_release=0; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ -n "$pin" && -n "$prefix" && -n "$build_dir" && -n "$toolchain" ]] ||
  die "usage: --pin <env> --prefix <dir> --build-dir <dir> --toolchain <file> [--source <dir>] [--no-release]"
[[ -f "$pin" ]] || die "pin file not found: $pin (configure Planar first)"
[[ -f "$toolchain" ]] || die "toolchain file not found: $toolchain"

# shellcheck source=/dev/null
source "$pin"
for v in CENTURION_TAG CENTURION_COMMIT CENTURION_SHA256 CENTURION_SOURCE_DIR; do
  [[ -n "${!v:-}" ]] || die "$pin does not define $v"
done
src="${source_override:-$CENTURION_SOURCE_DIR}"

share="$prefix/share/centurion"
mkdir -p "$prefix/bin" "$share"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }

# Replace <share>/migrations wholesale from a directory of *.up.sql/*.down.sql,
# so a migration dropped upstream does not linger from a previous install.
install_migrations() {
  local from="$1" staged="$share/migrations.new"
  compgen -G "$from/*.up.sql" >/dev/null || die "no *.up.sql migrations under $from"
  rm -rf "$staged"
  mkdir -p "$staged"
  cp "$from"/*.up.sql "$from"/*.down.sql "$staged"/
  rm -rf "$share/migrations"
  mv "$staged" "$share/migrations"
}

install_binary() {
  cp "$1" "$prefix/bin/centuriond.new"
  chmod 0755 "$prefix/bin/centuriond.new"
  mv "$prefix/bin/centuriond.new" "$prefix/bin/centuriond"
}

# --- 1. release binary ---------------------------------------------------------
from_release() {
  command -v gh >/dev/null 2>&1 || return 1
  local os arch asset
  os="$(uname -s | tr '[:upper:]' '[:lower:]')"
  arch="$(uname -m)"
  asset="centuriond-${CENTURION_TAG}-${os}-${arch}.tar.gz"
  gh release download "$CENTURION_TAG" --repo rdrsss/centurion \
    --pattern "$asset" --pattern "$asset.sha256" --dir "$tmp/release" \
    >/dev/null 2>&1 || return 1
  local want got
  want="$(awk '{print $1}' "$tmp/release/$asset.sha256")"
  got="$(sha256_of "$tmp/release/$asset")"
  [[ -n "$want" && "$want" == "$got" ]] ||
    die "release asset $asset does not match its published sha256 ($got != $want)"
  mkdir -p "$tmp/release/x"
  tar -xzf "$tmp/release/$asset" -C "$tmp/release/x"
  [[ -x "$tmp/release/x/bin/centuriond" ]] || die "$asset has no bin/centuriond"
  install_binary "$tmp/release/x/bin/centuriond"
  install_migrations "$tmp/release/x/share/centurion/migrations"
  say "installed release binary $asset"
}

# --- 2. source build -----------------------------------------------------------
from_source() {
  [[ -f "$src/CMakeLists.txt" ]] || die "Centurion source not found at $src (configure Planar first)"
  say "building centuriond ${CENTURION_TAG} from $src (first build compiles gRPC; expect minutes)"
  # A build tree remembers the source it was configured against, so a PIN BUMP
  # (a new tag is a new directory under external/) makes CMake refuse with
  # "does not match the source used to generate cache". Start that build over
  # rather than handing the operator that diagnostic: the old tree belongs to a
  # Centurion that is no longer pinned.
  local configured_source=""
  if [[ -f "$build_dir/CMakeCache.txt" ]]; then
    configured_source="$(awk -F= '/^CMAKE_HOME_DIRECTORY:/ {print $2}' "$build_dir/CMakeCache.txt")"
    if [[ -n "$configured_source" && "$configured_source" != "$src" ]]; then
      say "pin changed ($configured_source -> $src); reconfiguring $build_dir from scratch"
      rm -rf "$build_dir"
    fi
  fi
  # Normal-variable overrides only; the daemon's migrations default is baked
  # to the INSTALLED directory, never the source tree.
  cmake -S "$src" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCENTURION_BUILD_DAEMON=ON \
    -DCENTURION_BUILD_CLI=OFF \
    -DBUILD_TESTING=OFF \
    -DCENTURION_MIGRATIONS_PATH="$share/migrations" >"$tmp/configure.log" 2>&1 ||
    { cat "$tmp/configure.log" >&2; die "centuriond configure failed"; }
  cmake --build "$build_dir" --target centuriond ||
    die "centuriond build failed"
  # Not `cmake --install`: Centurion's EXCLUDE_FROM_ALL vendored subprojects
  # still contribute install rules for targets this build never compiled.
  local bin
  bin="$(find "$build_dir" -type f -name centuriond -perm -u+x -print -quit)"
  [[ -n "$bin" ]] || die "centuriond binary not found under $build_dir"
  install_binary "$bin"
  install_migrations "$src/migrations"
  say "installed source-built centuriond"
}

kind=""
if ((try_release)) && from_release; then
  kind="release-binary"
else
  from_source
  kind="source-build"
fi

cat >"$share/build-identity.json.new" <<EOF
{
  "tag": "${CENTURION_TAG}",
  "commit": "${CENTURION_COMMIT}",
  "archive_sha256": "${CENTURION_SHA256}",
  "source": "${kind}",
  "binary_sha256": "$(sha256_of "$prefix/bin/centuriond")",
  "migrations_dir": "${share}/migrations"
}
EOF
mv "$share/build-identity.json.new" "$share/build-identity.json"
say "wrote $share/build-identity.json (${kind}, ${CENTURION_TAG})"
