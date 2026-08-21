#!/usr/bin/env bash
#
# Download CPM.cmake into cmake/CPM.cmake.
#
# Pinned to the EXACT version + SHA256 of the CPM.cmake actually committed at
# cmake/CPM.cmake (read from its own CURRENT_CPM_VERSION fallback constant),
# fetched by its versioned GitHub release tag — never releases/latest, which
# is a moving target no hash can pin (task 6049, F8; mirrors
# cmake/dependencies.cmake's matching CMake-time bootstrap). CMake also
# bootstraps CPM automatically on first configure if cmake/CPM.cmake is
# missing, so running this by hand is optional — its main use is verifying
# (or re-fetching) the pinned version via --force.
#
# Usage:
#   scripts/get_cpm.sh            # download only if cmake/CPM.cmake is missing
#   scripts/get_cpm.sh --force    # re-fetch and re-verify the pinned version
#
# To bump the pin: update CPM_VERSION and CPM_SHA256 below together, verify
# the new hash independently against the release asset, then --force.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
dest="$repo_root/cmake/CPM.cmake"

CPM_VERSION="0.43.1"
CPM_SHA256="1c40fc102ce9625d7de7eb14f541cab30cc3138dca627f0b0ec40293ce6c2934"
url="https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"

force=0
if [[ "${1:-}" == "--force" ]]; then
  force=1
fi

if [[ -f "$dest" && "$force" -ne 1 ]]; then
  echo "CPM.cmake already present at $dest (use --force to re-fetch and re-verify)."
  exit 0
fi

mkdir -p "$(dirname "$dest")"
echo "Downloading pinned CPM.cmake v${CPM_VERSION} from $url"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
curl -fSL --retry 3 "$url" -o "$tmp"

got_sha256="$(shasum -a 256 "$tmp" | awk '{print $1}')"
if [[ "$got_sha256" != "$CPM_SHA256" ]]; then
  echo "get_cpm.sh: SHA256 mismatch for CPM.cmake v${CPM_VERSION}" >&2
  echo "  expected: $CPM_SHA256" >&2
  echo "  got:      $got_sha256" >&2
  exit 1
fi

mv "$tmp" "$dest"
echo "Installed CPM.cmake ${CPM_VERSION} (SHA256 verified) -> $dest"
