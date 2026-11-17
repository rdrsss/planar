#!/bin/sh
set -eu
python3 /opt/planar-retained-clean/verify.py > /logs/verifier/test-stdout.txt 2>&1
