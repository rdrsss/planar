#!/usr/bin/env bash
#
# Download CPM.cmake into cmake/CPM.cmake.
#
# By default this fetches the *latest* CPM.cmake release and only writes it if
# the file is not already present. CMake also bootstraps CPM automatically on
# first configure if cmake/CPM.cmake is missing (see cmake/dependencies.cmake),
# so running this by hand is optional — its main use is upgrading to a newer
# CPM release via --force.
#
# Usage:
#   scripts/get_cpm.sh            # download only if cmake/CPM.cmake is missing
#   scripts/get_cpm.sh --force    # always overwrite (upgrade to newest)
#
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
dest="$repo_root/cmake/CPM.cmake"
url="https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake"

force=0
if [[ "${1:-}" == "--force" ]]; then
  force=1
fi

if [[ -f "$dest" && "$force" -ne 1 ]]; then
  echo "CPM.cmake already present at $dest (use --force to upgrade)."
  exit 0
fi

mkdir -p "$(dirname "$dest")"
echo "Downloading latest CPM.cmake from $url"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
curl -fSL --retry 3 "$url" -o "$tmp"
mv "$tmp" "$dest"

version=$(sed -nE 's/.*CURRENT_CPM_VERSION[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$dest" | head -n1)
echo "Installed CPM.cmake ${version:-(unknown version)} -> $dest"
