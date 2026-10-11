#!/bin/bash
# Test-only bundle layout harness; assembly remains in dist.sh.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export PYTHONDONTWRITEBYTECODE=1
exec "${PYTHON:-python3}" "$root/scripts/dist-layout.test.py" "$@"
