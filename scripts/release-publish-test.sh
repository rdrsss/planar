#!/bin/bash
# Test-only harness for the release gates, the common publisher and make
# release-cut; the cases live in scripts/release-publish.test.py.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export PYTHONDONTWRITEBYTECODE=1
exec "${PYTHON:-python3}" "$root/scripts/release-publish.test.py" "$@"
