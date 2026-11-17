#!/bin/sh
set -eu
mkdir -p /logs/verifier
set +e
python3 /opt/planar-verifier/verify.py > /logs/verifier/status.json
status=$?
set -e
# Fail closed: neither an infrastructure error nor a behavioral failure passes.
if [ "$status" -eq 0 ]; then printf '1\n' > /logs/verifier/reward.txt; else printf '0\n' > /logs/verifier/reward.txt; fi
exit "$status"
