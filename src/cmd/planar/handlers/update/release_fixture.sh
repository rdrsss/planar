#!/usr/bin/env bash
# release_fixture.sh -- test-only: publish a fake Planar release into a local
# release directory laid out the way the GitHub release base is, so
# `planar update` can be driven through PLANAR_RELEASE_URL=file://<dir>
# (update_leaves.t.cpp; tech spec 677, "The update verb").
#
#   release_fixture.sh REPO REL TAG PLATFORM [SCHEMA] [LATEST]
#
#   REPO      the repository checkout (for scripts/fixtures/prebuilt-bundle.sh)
#   REL       the release base directory; created if missing
#   TAG       the release tag, written into the bundle's release.json
#   PLATFORM  macos-arm64 or linux-x86_64: the bundle is planar-PLATFORM/
#   SCHEMA    the bundle's release.json schema_version (default 41)
#   LATEST    yes (default) to point REL/latest/download/VERSION at TAG
#
# Produces REL/download/TAG/{planar-PLATFORM.tar.gz,SHA256SUMS} where
# SHA256SUMS also names a second platform's tarball and get-planar.sh that are
# not published, as the real merged manifest does. The bundle is the shared
# fake bundle (tiny stub binaries, the real install.sh and install-lib).
set -euo pipefail

repo="$1" rel="$2" tag="$3" platform="$4" schema="${5:-41}" latest="${6:-yes}"
# shellcheck source=../../../../../scripts/fixtures/prebuilt-bundle.sh
source "$repo/scripts/fixtures/prebuilt-bundle.sh"

work="$(mktemp -d "${TMPDIR:-/tmp}/planar-release-fixture.XXXXXX")"
trap 'rm -rf "$work"' EXIT
bundle="$work/planar-$platform"
fake_bundle_make "$repo" "$bundle"
os="${platform%%-*}" arch="${platform#*-}" floor=26.0
[[ "$os" == linux ]] && floor=2.36
printf '{\n  "version": "%s",\n  "sha": "%s",\n  "date": "2026-10-06T00:00:00Z",\n  "os": "%s",\n  "arch": "%s",\n  "os_floor": "%s",\n  "schema_version": %s\n}\n' \
  "$tag" "0123456789012345678901234567890123456789" "$os" "$arch" "$floor" "$schema" > "$bundle/release.json"
mkdir -p "$rel/download/$tag" "$rel/latest/download"
( cd "$work" && COPYFILE_DISABLE=1 tar -czf "$rel/download/$tag/planar-$platform.tar.gz" "planar-$platform" )
sum="$( (sha256sum "$rel/download/$tag/planar-$platform.tar.gz" 2>/dev/null || shasum -a 256 "$rel/download/$tag/planar-$platform.tar.gz") | cut -d' ' -f1)"
other=linux-x86_64
[[ "$platform" == linux-x86_64 ]] && other=macos-arm64
{
  printf '%s  planar-%s.tar.gz\n' "$(printf 'a%.0s' $(seq 1 64))" "$other"
  printf '%s  planar-%s.tar.gz\n' "$sum" "$platform"
  printf '%s  get-planar.sh\n' "$(printf 'b%.0s' $(seq 1 64))"
} > "$rel/download/$tag/SHA256SUMS"
if [[ "$latest" == yes ]]; then
  printf '%s\n' "$tag" > "$rel/latest/download/VERSION"
fi
