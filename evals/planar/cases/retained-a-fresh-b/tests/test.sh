#!/bin/sh
set -eu
python3 /opt/planar-retained/verify.py > /logs/verifier/test-stdout.txt 2>&1
