#!/bin/bash
# Build-time bundle assembly. Python is a build dependency, never bundled here.
set -euo pipefail
export LC_ALL=C
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail() { echo "dist: $*" >&2; exit 1; }

version=${PLANAR_RELEASE_VERSION:-dev}
if [ "$version" != dev ] || [ -n "${PLANAR_RELEASE_VERSION:-}" ]; then
  [[ "$version" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || fail "release must be a stable vMAJOR.MINOR.PATCH tag"
fi
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  sha=$(git rev-parse HEAD)
  dirty=0
  [ -z "$(git status --porcelain --untracked-files=all)" ] || dirty=1
  if [ "$version" != dev ]; then
    tag_sha=$(git rev-parse --verify "refs/tags/$version^{commit}") || fail "missing release tag $version"
    [ "$sha" = "$tag_sha" ] || fail "release tag $version requires HEAD at $tag_sha"
    [ "$dirty" = 0 ] || fail "release tag $version requires clean HEAD (dirty source)"
  fi
  [ -z "${PLANAR_SOURCE_SHA:-}" ] || [ "$PLANAR_SOURCE_SHA" = "$sha" ] || fail "supplied source SHA differs from HEAD"
  [ -z "${PLANAR_SOURCE_DIRTY:-}" ] || [ "$PLANAR_SOURCE_DIRTY" = "$dirty" ] || fail "supplied source dirty state differs from snapshot"
else
  # Trusted Docker wrapper supplies the exact source snapshot's identity.
  sha=${PLANAR_SOURCE_SHA:-}
  dirty=${PLANAR_SOURCE_DIRTY:-}
fi
[[ "$sha" =~ ^[0-9a-f]{40}$ ]] || fail "a full source SHA is required (git-free builds require trusted host identity)"
[[ "$dirty" =~ ^[01]$ ]] || fail "source dirty state must be 0 or 1"
[ "$version" = dev ] || [ "$dirty" = 0 ] || fail "tagged release requires clean source"
if [ "${1:-}" = --identity ]; then
  printf '%s\n%s\n' "$sha" "$dirty"
  exit 0
fi
[ "$#" = 0 ] || fail "usage: scripts/dist.sh [--identity]"
export PLANAR_SOURCE_SHA=$sha PLANAR_SOURCE_DIRTY=$dirty

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) os=macos; arch=arm64; floor=26.0 ;;
  Linux-x86_64) os=linux; arch=x86_64; floor=2.36 ;;
  *) fail "unsupported bundle platform: $(uname -s)-$(uname -m)" ;;
esac
build="$root/build/dist"
mkdir -p "$root/build"
scratch=$(mktemp -d "$root/build/dist-cut.XXXXXX")
arena=
trap 'rm -rf "$scratch"; if [ -n "$arena" ]; then rm -rf "$arena"; fi' EXIT
bundle="planar-$os-$arch"
stage="$scratch/$bundle"
logs="$root/build/dist-evidence"
mkdir -p "$logs"
cmake --preset dist -DPLANAR_RELEASE_VERSION="${PLANAR_RELEASE_VERSION:-}" \
  -DPLANAR_SOURCE_SHA="$sha" -DPLANAR_SOURCE_DIRTY="$dirty"
cmake --build "$build" --parallel "${JOBS:-4}"
# Fail closed if a portable check was unregistered: green zero-tests is no gate.
ctest --test-dir "$build" -L '^portable$' --show-only=json-v1 > "$scratch/tests.json"
python3 - "$scratch/tests.json" <<'PY'
import json, sys
names = {test['name'] for test in json.load(open(sys.argv[1]))['tests']}
if names != {'portable.binaries', 'portable.inspector'}:
    sys.exit(f'dist: expected both portable tests, found {sorted(names)}')
PY
ctest --test-dir "$build" -L '^portable$' --output-on-failure > "$logs/portable.log" 2>&1 || { cat "$logs/portable.log" >&2; fail "portable tests failed"; }
cat "$logs/portable.log"
cmake --install "$build" --prefix "$stage"
# Inspect the installed copies too; gate evidence binds these to the archive.
prefix=$(sed -n 's/^PLANAR_LLVM_PREFIX:PATH=//p' "$build/CMakeCache.txt")
[ -n "$prefix" ] || fail "missing portable toolchain prefix"
python3 scripts/portable-check.py --bin-dir "$stage/bin" --platform "$(uname -s)" --toolchain-prefix "$prefix" > "$logs/staged-portable.log" 2>&1 || { cat "$logs/staged-portable.log" >&2; fail "staged portability check failed"; }

mkdir -p "$stage/skills" "$stage/scripts"
python3 - "$stage" <<'PYTHON'
import pathlib, shutil, sys
stage = pathlib.Path(sys.argv[1])
ignore = shutil.ignore_patterns('__pycache__', '*.pyc', '.DS_Store')
for source, destination in [('skills/planar', 'skills/planar'), ('scripts/install-lib', 'scripts/install-lib'),
                            *[(name, name) for name in ('agents', 'templates', 'workflows', 'migrations')]]:
    shutil.copytree(source, stage / destination, symlinks=True, ignore=ignore)
PYTHON
cp install.sh install-cleanup.txt "$stage/"
# Later milestones supply these root assets. Never substitute another script.
for script in get-planar.sh uninstall.sh; do
  if [ -f "$script" ]; then cp "$script" "$stage/"; fi
done
python3 scripts/render-codex-agents.py "$stage/agents" "$stage/codex-agents"

# init is planning-class: even a scratch DB must be probed outside a worktree.
arena=$(mktemp -d "${TMPDIR:-/tmp}/dist-cut.XXXXXX")
arena=$(cd "$arena" && pwd -P)
export HOME="$arena" PLANAR_DB="$arena/planar.db" PLANAR_CONFIG_PATH="$arena/config.toml"
(cd "$arena"; unset GIT_DIR GIT_WORK_TREE; "$stage/bin/planar" init --skip-project --allow-no-repo) > "$logs/init.log" 2>&1 || { cat "$logs/init.log" >&2; fail "metadata initialization failed"; }
(cd "$arena"; unset GIT_DIR GIT_WORK_TREE; "$stage/bin/planar" health --json) > "$logs/health.json" || fail "metadata health failed"
for name in planar planar-agent planar-watch planar-ext; do
  "$stage/bin/$name" version --json > "$scratch/$name.json" || fail "$name version failed"
done
python3 - "$scratch" "$logs/health.json" "$stage/release.json" "$version" "$sha" "$dirty" "$os" "$arch" "$floor" <<'PY'
import json, pathlib, re, sys
scratch, health_path, output, version, sha, dirty, os_name, arch, floor = sys.argv[1:]
try:
    health = json.load(open(health_path))
    target, current = health['schema_target'], health['schema_version']
    def number(value):
        if isinstance(value, bool) or not re.fullmatch(r'[0-9]+', str(value)):
            raise ValueError('schema versions must be numeric integers')
        return int(value)
    if health['schema_current'] is not True or number(target) != number(current):
        raise ValueError('schema must be current and equal to target')
    metadata = [json.load(open(pathlib.Path(scratch) / (name + '.json')))
                for name in ('planar', 'planar-agent', 'planar-watch', 'planar-ext')]
    date = metadata[0]['date']
    if not re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z', date):
        raise ValueError('invalid embedded build date')
    for entry in metadata:
        if (entry['release'], entry['sha'], entry['dirty'], entry['date']) != (version, sha, dirty == '1', date):
            raise ValueError('binary metadata differs from source identity')
    release = dict(version=version, sha=sha, date=date, os=os_name, arch=arch,
                   os_floor=floor, schema_version=target)
    pathlib.Path(output).write_text(json.dumps(release, indent=2) + '\n')
except (ValueError, KeyError, OSError, TypeError) as error:
    sys.exit(f'dist: invalid bundle metadata: {error}')
PY
# Recheck native source after the build, before emitting any archive.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  actual=$(PLANAR_SOURCE_SHA="$sha" PLANAR_SOURCE_DIRTY="$dirty" scripts/dist.sh --identity)
  [ "$actual" = "$(printf '%s\n%s' "$sha" "$dirty")" ] || fail "source identity changed during build"
fi
mkdir -p dist
python3 - "$stage" "$scratch" "$root/dist" "$logs" <<'PY'
import hashlib, json, pathlib, sys, tarfile
stage, scratch, output, logs = map(pathlib.Path, sys.argv[1:])
archive = scratch / (stage.name + '.tar.gz')
with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as tar:
    for path in sorted([stage, *stage.rglob('*')], key=lambda p: p.relative_to(stage.parent).as_posix()):
        info = tar.gettarinfo(str(path), arcname=path.relative_to(stage.parent).as_posix())
        info.uid = info.gid = 0
        info.uname = info.gname = 'root'
        if info.isfile():
            with path.open('rb') as source:
                tar.addfile(info, source)
        else:
            tar.addfile(info)
digest = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(archive.read_bytes()).hexdigest()
release = json.loads((stage / 'release.json').read_text())
evidence = dict(format_version=1, archive=archive.name, sha256=digest, release=release,
                gates=dict(portable=dict(result='pass', matched_count=2,
                                         staged_binaries=5)))
# Smoke/CA gates are deliberately absent; this alone cannot authorize publication.
archive.replace(output / archive.name)
(output / (archive.name + '.gates.json')).write_text(json.dumps(evidence, indent=2) + '\n')
# Preserve the other platform's checksum when assembling in a shared output dir.
checksums = []
for asset in sorted(output.glob('planar-*.tar.gz')):
    checksums.append(hashlib.sha256(asset.read_bytes()).hexdigest() + '  ' + asset.name + '\n')
(output / 'SHA256SUMS').write_text(''.join(checksums))
(output / 'VERSION').write_text(release['version'] + '\n')
print(f'dist: {output / archive.name} ({digest})')
PY
