#!/bin/bash
# release-publish.sh -- publish one release from an existing stable tag (plan
# 1122, task rel-release-publish; tech spec 677, "The release pipeline").
#
# Usage:
#   scripts/release-publish.sh [--dry-run] <vMAJOR.MINOR.PATCH> <dir>...
#   scripts/release-publish.sh --preflight <vMAJOR.MINOR.PATCH>
#
# Each <dir> holds release bundles (planar-<platform>.tar.gz) with the release
# evidence scripts/release-gates.sh wrote beside them, and optionally the
# standalone get-planar.sh that scripts/dist.sh emits. The common gate, applied the
# same way with and without --dry-run, refuses (exit 1) unless:
#
#   - the tag is a stable vMAJOR.MINOR.PATCH (a pre-release is never cut), exists
#     as an annotated tag (its annotation becomes the release notes) and HEAD is
#     at its commit; this script never creates a tag;
#   - there is exactly one macos-arm64 and one linux-x86_64 bundle, each named for
#     the platform its release.json declares, with that platform's OS floor;
#   - both release.json files carry the tag as version and the tag's commit as
#     sha, and the same schema_version;
#   - each archive has format_version 2 evidence whose sha256 is the archive's
#     SHA-256 now, whose release identity is the bundled release.json, and whose
#     required gates (portable and smoke; ca_debian and ca_redhat on Linux) all
#     passed with a matched count no smaller than the gate's complete size, which
#     this script owns (portable 2, smoke 7, ca_debian 2, ca_redhat 1). smoke and
#     both CA gates must record an expected_count equal to the matched count, and
#     the CA gates exactly the cases debian+removed and redhat;
#   - both bundled get-planar.sh copies, and every standalone copy found, are
#     byte-identical to scripts/get-planar.sh at the tag commit.
#
# It then stages the five public assets under dist/release/<tag>/ -- both
# tarballs, a merged SHA256SUMS (two-space records for both tarballs and
# get-planar.sh), VERSION and get-planar.sh -- and runs
# `gh release create <tag> --verify-tag --notes-from-tag` with them. --dry-run
# performs every check, then prints the per-platform gate counts, SHA256SUMS,
# VERSION and the exact command, and writes and runs nothing. On any refusal it
# prints no command. --preflight checks only the tag, HEAD and a clean tree, so
# `make release-cut` can refuse before building; it prints the tag commit.
set -euo pipefail
export LC_ALL=C
root=$(cd "$(dirname "$0")/.." && pwd)
fail() { echo "release-publish: $*" >&2; exit 1; }
usage() {
  echo "usage: scripts/release-publish.sh [--dry-run] <vMAJOR.MINOR.PATCH> <dir>..." >&2
  echo "       scripts/release-publish.sh --preflight <vMAJOR.MINOR.PATCH>" >&2
  exit 2
}

mode=publish
case "${1:-}" in
  --dry-run) mode=dry-run; shift ;;
  --preflight) mode=preflight; shift ;;
  -*) usage ;;
esac
[ "$#" -ge 1 ] || usage
tag=$1
shift
if [ "$mode" = preflight ]; then
  [ "$#" = 0 ] || usage
else
  [ "$#" -ge 1 ] || usage
fi

if [[ "$tag" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  :
elif [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+[-+] ]]; then
  fail "pre-release tag $tag refused: releases are stable vMAJOR.MINOR.PATCH tags and pre-releases are never cut"
else
  fail "release tag $tag is not a stable vMAJOR.MINOR.PATCH tag"
fi

git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "$root is not a git checkout"
tag_sha=$(git -C "$root" rev-parse --verify --quiet "refs/tags/$tag^{commit}") ||
  fail "missing release tag $tag; release-publish never creates tags"
[ "$(git -C "$root" cat-file -t "refs/tags/$tag")" = tag ] ||
  fail "release tag $tag is lightweight; an annotated tag supplies the release notes"
head=$(git -C "$root" rev-parse HEAD)
[ "$head" = "$tag_sha" ] || fail "HEAD is at $head but release tag $tag is at $tag_sha"
if [ "$mode" = preflight ]; then
  [ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ] ||
    fail "release tag $tag requires a clean checkout at $tag_sha (dirty source)"
  echo "$tag_sha"
  exit 0
fi

dirs=()
for dir in "$@"; do
  [ -d "$dir" ] || fail "not a directory: $dir"
  dirs+=("$(cd "$dir" && pwd)")
done
if [ "$mode" = publish ]; then
  command -v gh >/dev/null 2>&1 || fail "gh is required to publish (use --dry-run to validate only)"
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/release-publish.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
git -C "$root" show "$tag_sha:scripts/get-planar.sh" > "$scratch/get-planar.sh" ||
  fail "release tag $tag has no scripts/get-planar.sh"
stage="$root/dist/release/$tag"

python3 - "$mode" "$tag" "$tag_sha" "$stage" "$scratch" "${dirs[@]}" <<'PY'
import hashlib, json, pathlib, shlex, shutil, sys, tarfile
mode, tag, tag_sha, stage, scratch, *dirs = sys.argv[1:]
stage, scratch = pathlib.Path(stage), pathlib.Path(scratch)
# Each platform's required gates and the size of each: the number of checks a
# complete run performs. portable is the two portable ctest cases scripts/dist.sh
# requires; smoke is the seven checks of scripts/release-smoke.sh; ca_debian is
# trusted plus refused-once-removed and ca_redhat is one trusted case, both from
# scripts/release-gates.sh. The publisher owns these numbers so that evidence
# which ran fewer checks than a complete run is refused even when it says pass.
PORTABLE, SMOKE = dict(size=2), dict(size=7, expected=True)
CA_DEBIAN = dict(size=2, expected=True, cases=['debian', 'removed'])
CA_REDHAT = dict(size=1, expected=True, cases=['redhat'])
PLATFORMS = {'macos-arm64': dict(floor='26.0', gates=dict(portable=PORTABLE, smoke=SMOKE)),
             'linux-x86_64': dict(floor='2.36', gates=dict(portable=PORTABLE, smoke=SMOKE,
                                                           ca_debian=CA_DEBIAN, ca_redhat=CA_REDHAT))}

def fail(message):
    sys.exit(f'release-publish: {message}')

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

bootstrap = (scratch / 'get-planar.sh').read_bytes()
bundles = {}
for directory in map(pathlib.Path, dirs):
    standalone = directory / 'get-planar.sh'
    if standalone.exists() and standalone.read_bytes() != bootstrap:
        fail(f'{standalone} differs from {tag}:scripts/get-planar.sh')
    for archive in sorted(directory.glob('planar-*.tar.gz')):
        name = archive.name[:-len('.tar.gz')]
        try:
            with tarfile.open(archive) as tar:
                release = json.load(tar.extractfile(f'{name}/release.json'))
                copy = tar.extractfile(f'{name}/get-planar.sh').read()
        except (OSError, KeyError, AttributeError, ValueError, tarfile.TarError) as error:
            fail(f'{archive}: not a readable release bundle: {error}')
        if not isinstance(release, dict):
            fail(f'{archive}: release.json is not an object')
        platform = f'{release.get("os")}-{release.get("arch")}'
        if platform not in PLATFORMS:
            fail(f'{archive}: unsupported bundle platform {platform}')
        if archive.name != f'planar-{platform}.tar.gz':
            fail(f'{archive}: release.json declares {platform}, the archive name does not')
        if platform in bundles:
            fail(f'duplicate {platform} bundles: {bundles[platform]["archive"]} and {archive}')
        bundles[platform] = dict(archive=archive, release=release, bootstrap=copy)

missing = sorted(set(PLATFORMS) - set(bundles))
if missing:
    fail(f'missing platform bundle: {", ".join(missing)} (a release ships exactly '
         f'{" and ".join(sorted(PLATFORMS))})')
order = sorted(bundles)
for platform in order:
    bundle = bundles[platform]
    version = bundle['release'].get('version')
    if version != tag:
        fail(f'{bundle["archive"]}: bundle version {version} differs from release tag {tag}')
shas = {platform: bundles[platform]['release'].get('sha') for platform in order}
if len(set(shas.values())) != 1:
    fail('mixed bundle commits: ' + ', '.join(f'{p} at {s}' for p, s in shas.items()))
if shas[order[0]] != tag_sha:
    fail(f'bundles were built from {shas[order[0]]} but release tag {tag} is at {tag_sha}')
schemas = {platform: bundles[platform]['release'].get('schema_version') for platform in order}
if len({json.dumps(value) for value in schemas.values()}) != 1:
    fail('mixed bundle schemas: ' + ', '.join(f'{p} schema {json.dumps(s)}' for p, s in schemas.items()))
for platform in order:
    bundle, rule = bundles[platform], PLATFORMS[platform]
    archive = bundle['archive']
    floor = bundle['release'].get('os_floor')
    if floor != rule['floor']:
        fail(f'{archive}: os_floor {floor} is not the {platform} floor {rule["floor"]}')
    if bundle['bootstrap'] != bootstrap:
        fail(f'{archive}: bundled get-planar.sh differs from {tag}:scripts/get-planar.sh')
    evidence = archive.with_name(archive.name + '.gates.json')
    if not evidence.is_file():
        fail(f'{archive}: missing gate evidence {evidence.name}')
    try:
        record = json.loads(evidence.read_text())
    except (OSError, ValueError) as error:
        fail(f'{evidence}: unreadable gate evidence: {error}')
    if not isinstance(record, dict) or record.get('format_version') != 2:
        fail(f'{evidence}: format_version {record.get("format_version") if isinstance(record, dict) else None} '
             'is not release evidence; run scripts/release-gates.sh on this archive')
    if record.get('archive') != archive.name or record.get('platform') != platform:
        fail(f'{evidence}: evidence names {record.get("archive")} ({record.get("platform")}), '
             f'not {archive.name} ({platform})')
    digest = sha256(archive)
    if record.get('sha256') != digest:
        fail(f'{archive}: sha256 {digest} differs from its gate evidence {record.get("sha256")}; '
             'the archive changed after its gates ran')
    if record.get('release') != bundle['release']:
        fail(f'{evidence}: evidence release identity differs from {archive.name} release.json')
    gates = record.get('gates') if isinstance(record.get('gates'), dict) else {}
    summary = []
    for name, need in rule['gates'].items():
        gate = gates.get(name)
        if not isinstance(gate, dict):
            fail(f'{archive}: missing {name} gate evidence')
        if gate.get('result') != 'pass':
            fail(f'{archive}: {name} gate result is {gate.get("result")}, not pass')
        count = gate.get('matched_count')
        if isinstance(count, bool) or not isinstance(count, int) or count < 1:
            fail(f'{archive}: {name} gate matched {count!r} checks; zero matched is no gate')
        expected = gate.get('expected_count')
        if expected is None and need.get('expected'):
            fail(f'{archive}: {platform} {name} gate matched {count} checks and records no expected_count; '
                 f'missing checks (a complete {platform} {name} gate runs {need["size"]})')
        if expected is not None:
            if isinstance(expected, bool) or not isinstance(expected, int):
                fail(f'{archive}: {platform} {name} gate expected_count {expected!r} is not a count')
            if expected != count:
                fail(f'{archive}: {platform} {name} gate matched {count} of {expected} expected checks')
        if count < need['size']:  # expected_count, when recorded, equals count
            fail(f'{archive}: {platform} {name} gate matched {count} of '
                 f'{"no recorded" if expected is None else expected} expected checks, below the '
                 f'{need["size"]} a complete {platform} {name} gate runs; missing checks')
        if 'cases' in need and gate.get('cases') != need['cases']:
            fail(f'{archive}: {platform} {name} gate ran cases {gate.get("cases")!r}, not {need["cases"]!r} '
                 f'(matched {count} of {expected} expected checks); missing checks')
        summary.append(f'{name}={count}' if expected is None else f'{name}={count}/{expected}')
    if gates['portable'].get('staged_binaries') != 5:
        fail(f'{archive}: portable gate inspected {gates["portable"].get("staged_binaries")!r} staged binaries, not 5')
    bundle['summary'] = summary
    bundle['digest'] = digest

# The public asset set: both tarballs, SHA256SUMS, VERSION and get-planar.sh.
tarballs = [bundles[platform]['archive'] for platform in order]
records = [f'{bundles[p]["digest"]}  {bundles[p]["archive"].name}\n' for p in order]
records.append(f'{hashlib.sha256(bootstrap).hexdigest()}  get-planar.sh\n')
checksums = ''.join(sorted(records, key=lambda line: line.split('  ', 1)[1]))
assets = [stage / archive.name for archive in tarballs] + [stage / name for name in ('SHA256SUMS', 'VERSION', 'get-planar.sh')]
command = ['gh', 'release', 'create', tag, '--verify-tag', '--title', tag, '--notes-from-tag', *map(str, assets)]
for platform in order:
    print(f'release-publish: {platform} {bundles[platform]["archive"].name} gates: {" ".join(bundles[platform]["summary"])}')
if mode == 'dry-run':
    print(f'release-publish: dry run for {tag} at {tag_sha}; nothing was staged or published')
    print('--- SHA256SUMS')
    print(checksums, end='')
    print('--- VERSION')
    print(tag)
    print('--- command')
    print(shlex.join(command))
    sys.exit(0)

if stage.exists():
    shutil.rmtree(stage)
stage.mkdir(parents=True)
for platform, archive in zip(order, tarballs):
    shutil.copyfile(archive, stage / archive.name)
    if sha256(stage / archive.name) != bundles[platform]['digest']:
        fail(f'{archive.name}: staged copy differs from the gated archive')
(stage / 'SHA256SUMS').write_text(checksums)
(stage / 'VERSION').write_text(tag + '\n')
(stage / 'get-planar.sh').write_bytes(bootstrap)
(stage / 'get-planar.sh').chmod(0o755)
(scratch / 'assets').write_text(''.join(str(path) + '\n' for path in assets))
print(f'release-publish: staged {len(assets)} assets in {stage}')
PY

[ "$mode" = publish ] || exit 0
assets=()
while IFS= read -r asset; do
  assets+=("$asset")
done < "$scratch/assets"
[ "${#assets[@]}" = 5 ] || fail "expected five staged assets, found ${#assets[@]}"
echo "release-publish: gh release create $tag --verify-tag --title $tag --notes-from-tag (${#assets[@]} assets)"
(cd "$root" && gh release create "$tag" --verify-tag --title "$tag" --notes-from-tag "${assets[@]}") ||
  fail "gh release create failed for $tag; the staged assets remain in $stage"
echo "release-publish: published $tag"
