#!/bin/bash
# release-gates.sh -- run one platform's clean-host release gates and bind the
# result to the exact archive (plan 1122, task rel-release-publish; tech spec
# 677, "The release pipeline").
#
# Usage: scripts/release-gates.sh --platform <macos-arm64|linux-x86_64> <dir>
#
# <dir> holds planar-<platform>.tar.gz and the assembly evidence that
# scripts/dist.sh wrote beside it (planar-<platform>.tar.gz.gates.json,
# format_version 1: the portable gate only). This script checks that evidence
# against the archive's SHA-256 and its release.json, extracts the archive and
# runs the gates on the extracted binaries:
#
#   smoke      scripts/release-smoke.sh. macos-arm64: on this macOS arm64 host
#              under `env -i` with a system-only PATH (no toolchain, no DYLD_*).
#              linux-x86_64: in a bare $PLANAR_GATE_RUNTIME_IMAGE container
#              (default debian:bookworm-slim, linux/amd64, no network).
#   ca_debian  linux-x86_64 only, scripts/test-portable-tls.py: trusted through
#              the Debian /etc/ssl/certs directory, and refused once that CA is
#              removed (two cases).
#   ca_redhat  linux-x86_64 only: trusted through /etc/pki/tls/certs/ca-bundle.crt.
#
# It then rewrites the evidence file as format_version 2, the release evidence
# that scripts/release-publish.sh requires: the archive name, its SHA-256 (checked
# again after the gates ran), the platform, the full release identity and one
# record per gate with its result and actual matched count. Logs go to
# <dir>/planar-<platform>.gate-logs/. Exit 0 only when every gate passed; a failed
# gate is still recorded, and the publisher refuses it. Rebuilding the archive with
# scripts/dist.sh replaces the evidence with format 1 again, so a new archive
# always needs a new gate run.
set -euo pipefail
export LC_ALL=C
root=$(cd "$(dirname "$0")/.." && pwd)
fail() { echo "release-gates: $*" >&2; exit 1; }
usage() { echo "usage: scripts/release-gates.sh --platform <macos-arm64|linux-x86_64> <dir>" >&2; exit 2; }

[ "$#" = 3 ] && [ "$1" = --platform ] || usage
platform=$2
case "$platform" in
  macos-arm64 | linux-x86_64) ;;
  *) fail "unsupported release platform: $platform" ;;
esac
[ -d "$3" ] || fail "not a directory: $3"
dir=$(cd "$3" && pwd)
archive="$dir/planar-$platform.tar.gz"
evidence="$archive.gates.json"
[ -f "$archive" ] || fail "missing archive $archive"
[ -f "$evidence" ] || fail "missing assembly evidence $evidence (scripts/dist.sh writes it)"

runtime_image=${PLANAR_GATE_RUNTIME_IMAGE:-debian:bookworm-slim}
if [ "$platform" = macos-arm64 ]; then
  host="$(uname -s)-$(uname -m)"
  [ "$host" = Darwin-arm64 ] || fail "macos-arm64 gates run on a macOS arm64 host (this host is $host)"
  environment="macos env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin"
else
  command -v docker >/dev/null 2>&1 || fail "linux-x86_64 gates need docker for their clean containers"
  environment="docker $runtime_image linux/amd64"
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/release-gates.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
logs="$dir/planar-$platform.gate-logs"
rm -rf "$logs"
mkdir -p "$logs"

# Check the assembly evidence against the archive before running anything, and
# extract the bundle the gates run.
python3 - "$archive" "$evidence" "$platform" "$scratch" > "$scratch/digest" <<'PY'
import hashlib, json, pathlib, sys, tarfile
archive, evidence, platform, scratch = sys.argv[1:]
archive, scratch = pathlib.Path(archive), pathlib.Path(scratch)
def fail(message):
    sys.exit(f'release-gates: {message}')
try:
    record = json.loads(pathlib.Path(evidence).read_text())
except (OSError, ValueError) as error:
    fail(f'{evidence}: unreadable evidence: {error}')
if not isinstance(record, dict) or record.get('format_version') not in (1, 2):
    fail(f'{evidence}: unknown evidence format')
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
if record.get('archive') != archive.name:
    fail(f'{evidence}: names archive {record.get("archive")!r}, not {archive.name}')
if record.get('sha256') != digest:
    fail(f'{archive.name}: sha256 {digest} differs from its evidence {record.get("sha256")}; '
         'the archive changed after assembly')
portable = (record.get('gates') or {}).get('portable')
count = portable.get('matched_count') if isinstance(portable, dict) else None
if not isinstance(portable, dict) or portable.get('result') != 'pass':
    fail(f'{archive.name}: the portable gate did not pass')
if isinstance(count, bool) or not isinstance(count, int) or count < 1:
    fail(f'{archive.name}: the portable gate matched {count!r} tests; zero matched is no gate')
if portable.get('staged_binaries') != 5:
    fail(f'{archive.name}: the portable gate inspected {portable.get("staged_binaries")!r} staged binaries, not 5')
bundle = archive.name[:-len('.tar.gz')]
with tarfile.open(archive) as tar:
    try:
        release = json.load(tar.extractfile(f'{bundle}/release.json'))
    except (KeyError, AttributeError, ValueError) as error:
        fail(f'{archive.name}: no readable {bundle}/release.json: {error}')
    if record.get('release') != release:
        fail(f'{archive.name}: evidence release identity differs from the bundled release.json')
    if f'{release.get("os")}-{release.get("arch")}' != platform:
        fail(f'{archive.name}: release.json names {release.get("os")}-{release.get("arch")}, not {platform}')
    if hasattr(tarfile, 'data_filter'):
        tar.extractall(scratch, filter='data')
    else:
        tar.extractall(scratch)
print(digest)
PY
bundle="$scratch/planar-$platform"

# smoke: the release binaries run without the developer toolchain.
echo "release-gates: $platform smoke ($environment)"
smoke_rc=0
if [ "$platform" = macos-arm64 ]; then
  env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="${TMPDIR:-/tmp}" \
    /bin/sh "$root/scripts/release-smoke.sh" "$bundle" > "$logs/smoke.log" 2>&1 || smoke_rc=$?
else
  docker run --rm --platform linux/amd64 --network none \
    -v "$bundle:/opt/planar-bundle:ro" \
    -v "$root/scripts/release-smoke.sh:/opt/planar-gates/release-smoke.sh:ro" \
    "$runtime_image" sh /opt/planar-gates/release-smoke.sh /opt/planar-bundle \
    > "$logs/smoke.log" 2>&1 || smoke_rc=$?
fi
cat "$logs/smoke.log"

# ca_debian and ca_redhat: the portable Linux binaries trust the host CA store.
ca_rc=none
if [ "$platform" = linux-x86_64 ]; then
  toolchain_image=${PLANAR_GATE_TOOLCHAIN_IMAGE:-}
  ca_rc=0
  if [ -z "$toolchain_image" ]; then
    # The TLS fixture needs Python and OpenSSL: the bookworm dist toolchain stage.
    toolchain_image=planar-release-toolchain:local
    echo "release-gates: building $toolchain_image (docker/linux-gate.Dockerfile dist-toolchain)"
    DOCKER_BUILDKIT=1 docker build --platform linux/amd64 --target dist-toolchain \
      -f "$root/docker/linux-gate.Dockerfile" -t "$toolchain_image" "$root" \
      > "$logs/toolchain-image.log" 2>&1 || ca_rc=$?
  fi
  if [ "$ca_rc" = 0 ]; then
    echo "release-gates: $platform CA trust (scripts/test-portable-tls.py)"
    python3 "$root/scripts/test-portable-tls.py" --bin-dir "$bundle/bin" --evidence "$logs/ca" \
      --toolchain-image "$toolchain_image" --runtime-image "$runtime_image" \
      --platform linux/amd64 > "$logs/ca.log" 2>&1 || ca_rc=$?
    cat "$logs/ca.log"
  else
    cat "$logs/toolchain-image.log" >&2
  fi
fi

# Record the release evidence, bound to the archive as it is now.
python3 - "$archive" "$evidence" "$platform" "$(cat "$scratch/digest")" "$logs" \
  "$smoke_rc" "$ca_rc" "$environment" <<'PY'
import datetime, hashlib, json, os, pathlib, re, sys
archive, evidence, platform, digest, logs, smoke_rc, ca_rc, environment = sys.argv[1:]
archive, evidence, logs = pathlib.Path(archive), pathlib.Path(evidence), pathlib.Path(logs)
record = json.loads(evidence.read_text())
if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
    sys.exit(f'release-gates: {archive.name} changed while its gates ran; no evidence recorded')
gates = dict(portable=record['gates']['portable'])

smoke_log = (logs / 'smoke.log').read_text(errors='replace')
expected = re.findall(r'^smoke_expected=([0-9]+)$', smoke_log, re.M)
expected = int(expected[0]) if len(expected) == 1 else 0
passed = len(re.findall(r'^smoke_check=\S+ result=pass$', smoke_log, re.M))
gates['smoke'] = dict(result='pass' if smoke_rc == '0' and expected > 0 and passed == expected else 'fail',
                      matched_count=passed, expected_count=expected, environment=environment,
                      log=str((logs / 'smoke.log').relative_to(archive.parent)))

if ca_rc != 'none':
    ca = logs / 'ca'
    def case(mode):
        path = ca / f'tls-{mode}.log'
        return path.is_file() and re.search(r'^probe_exit=0$', path.read_text(errors='replace'), re.M) is not None
    server = ca / 'tls-server.log'
    rejected = server.is_file() and 'TLS_HANDSHAKE_REJECTION=TLSV1_ALERT_UNKNOWN_CA' in server.read_text(errors='replace')
    debian = [case('debian'), case('removed') and rejected]
    redhat = [case('redhat')]
    # Each gate stands on its own cases; a failed run with every case passing
    # (the image build, the fixture server) fails both.
    unexplained = ca_rc != '0' and all(debian + redhat)
    for name, cases, modes in (('ca_debian', debian, ['debian', 'removed']), ('ca_redhat', redhat, ['redhat'])):
        gates[name] = dict(result='pass' if all(cases) and not unexplained else 'fail',
                           matched_count=sum(cases), expected_count=len(cases), cases=modes,
                           log=str((logs / 'ca.log').relative_to(archive.parent)))

record = dict(format_version=2, archive=archive.name, sha256=digest, platform=platform,
              release=record['release'], gates=gates,
              recorded_at=datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))
temporary = evidence.with_name(evidence.name + '.tmp')
temporary.write_text(json.dumps(record, indent=2) + '\n')
os.replace(temporary, evidence)
failed = sorted(name for name, gate in gates.items() if gate['result'] != 'pass')
for name, gate in gates.items():
    count = f'{gate["matched_count"]}/{gate["expected_count"]}' if 'expected_count' in gate else str(gate['matched_count'])
    print(f'release-gates: {platform} {name} {gate["result"]} {count}')
if failed:
    sys.exit(f'release-gates: {platform} failed gates: {", ".join(failed)} (evidence {evidence})')
print(f'release-gates: {platform} evidence bound to {archive.name} {digest}')
PY
