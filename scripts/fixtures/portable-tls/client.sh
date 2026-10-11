#!/bin/sh
set -eu
export HOME=/arena/home PLANAR_DB=/arena/planar.db PLANAR_CONFIG_PATH=/arena/config.toml TLS_TOKEN=fixture-token
mkdir -p "$HOME" /arena/project
cd /arena/project
/opt/planar/planar version
/opt/planar/planar init --allow-no-repo --name fixture --slug fixture
/opt/planar/planar health --json
/opt/planar/planar plan create 'Local TLS plan' --scope global --json > /arena/plan.json
cat /arena/plan.json
plan_id=$(sed -n 's/.*"id": *\([0-9][0-9]*\).*/\1/p' /arena/plan.json | head -1)
test -n "$plan_id"
/opt/planar/planar-ext ext register jira tls --base-url https://localhost:4443 --project TLS --auth-env TLS_TOKEN --json
/opt/planar/planar link "plan:$plan_id" --to tls:TLS-1 --role mirror --sync read-only --json
set +e
/opt/planar/planar-ext sync pull --all --system tls --json > /arena/pull.json 2>&1
rc=$?
set -e
cat /arena/pull.json
printf 'request_exit=%s\n' "$rc"
case "$1" in
  trusted) test "$rc" = 0; grep -q '"remote_title":"TLS fixture"' /arena/pull.json ;;
  untrusted) ! grep -q '"remote_title"' /arena/pull.json; test "$rc" = 0; grep -q '"outcome":"error"' /arena/pull.json; grep -q '"detail":"CertificateVerificationFailed"' /arena/pull.json ;;
esac
