#!/usr/bin/env python3
"""Write one fake release bundle the way scripts/dist.sh lays it out.

Usage: make-bundle.py <out-dir> <platform> <version> <sha> <bootstrap-path>

Test fixture for scripts/release-publish.test.py only. The products are tiny sh
scripts that answer scripts/release-smoke.sh inside its scratch arena and refuse
anywhere else. Per-platform knobs come from FAKE_<KNOB>_<platform with _>
environment variables: VERSION, SHA, BINARY_SHA, SCHEMA, FLOOR, HEALTH_BROKEN,
PORTABLE_MATCHED, BOOTSTRAP_EXTRA and STANDALONE_EXTRA.
"""
import hashlib
import json
import os
import pathlib
import sys
import tarfile
import tempfile

out, platform, version, sha, bootstrap_path = sys.argv[1:]
out = pathlib.Path(out)
out.mkdir(parents=True, exist_ok=True)
key = platform.replace('-', '_')


def knob(name, default=''):
    return os.environ.get(f'FAKE_{name}_{key}', default)


os_name, arch = platform.split('-')
version = knob('VERSION', version)
sha = knob('SHA', sha)
schema = int(knob('SCHEMA', '41'))
floor = knob('FLOOR', {'macos-arm64': '26.0', 'linux-x86_64': '2.36'}[platform])
current = 'false' if knob('HEALTH_BROKEN') else 'true'
binary_sha = knob('BINARY_SHA', sha)
release = dict(version=version, sha=sha, date='2026-10-07T00:00:00Z', os=os_name, arch=arch,
               os_floor=floor, schema_version=schema)
bootstrap = pathlib.Path(bootstrap_path).read_bytes()
binary = f'''#!/bin/sh
# Fixture product: it answers only inside a release-smoke arena with a system PATH.
case "$HOME" in */planar-smoke.*/home) ;; *) echo "fixture: HOME is not a smoke arena: $HOME" >&2; exit 97 ;; esac
[ "$PLANAR_DB" = "${{HOME%/home}}/planar.db" ] || {{ echo "fixture: PLANAR_DB is outside the arena" >&2; exit 97; }}
[ "$PATH" = /usr/bin:/bin:/usr/sbin:/sbin ] || {{ echo "fixture: PATH is not system-only: $PATH" >&2; exit 97; }}
case "$1" in
  version) printf '{{"release":"{version}","sha":"{binary_sha}","date":"2026-10-07T00:00:00Z","dirty":false,"compiler":"fixture"}}\\n' ;;
  --help) echo "fixture usage" ;;
  init) : > "$PLANAR_DB" ;;
  health) [ -f "$PLANAR_DB" ] || exit 3
          printf '{{"schema_version":{schema},"schema_target":{schema},"schema_current":{current}}}\\n' ;;
  *) exit 2 ;;
esac
'''
name = f'planar-{platform}'
with tempfile.TemporaryDirectory() as temporary:
    stage = pathlib.Path(temporary) / name
    (stage / 'bin').mkdir(parents=True)
    (stage / 'release.json').write_text(json.dumps(release, indent=2) + '\n')
    (stage / 'get-planar.sh').write_bytes(bootstrap + knob('BOOTSTRAP_EXTRA').encode())
    for product in ('planar', 'planar-agent', 'planar-watch', 'planar-ext', 'planar-execute'):
        (stage / 'bin' / product).write_text(binary)
        (stage / 'bin' / product).chmod(0o755)
    archive = out / f'{name}.tar.gz'
    with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as tar:
        for path in sorted([stage, *stage.rglob('*')], key=lambda p: p.relative_to(stage.parent).as_posix()):
            tar.add(path, arcname=path.relative_to(stage.parent).as_posix(), recursive=False)
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
evidence = dict(format_version=1, archive=archive.name, sha256=digest, release=release,
                gates=dict(portable=dict(result='pass', matched_count=int(knob('PORTABLE_MATCHED', '2')),
                                         staged_binaries=5)))
(out / (archive.name + '.gates.json')).write_text(json.dumps(evidence, indent=2) + '\n')
(out / 'get-planar.sh').write_bytes(bootstrap + knob('STANDALONE_EXTRA').encode())
(out / 'get-planar.sh').chmod(0o755)
(out / 'SHA256SUMS').write_text(f'{digest}  {archive.name}\n')
(out / 'VERSION').write_text(version + '\n')
print(f'fixture bundle: {archive} {digest}')
