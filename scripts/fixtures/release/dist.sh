#!/bin/bash
# Fake scripts/dist.sh for the release-cut fixture repository: identity as the
# real one reports it for a clean checkout, and a fake native macOS bundle.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
if [ "${1:-}" = --identity ]; then
  git rev-parse HEAD
  echo 0
  exit 0
fi
echo "fake-dist ${PLANAR_RELEASE_VERSION:-dev}" >> "$FAKE_LOG"
exec python3 "$FAKE_TOOLS/make-bundle.py" dist macos-arm64 "${PLANAR_RELEASE_VERSION:-dev}" \
  "$(git rev-parse HEAD)" scripts/get-planar.sh
