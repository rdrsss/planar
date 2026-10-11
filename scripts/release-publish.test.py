#!/usr/bin/env python3
"""The release gates, the common publisher and make release-cut, against fakes.

Plan 1122, task rel-release-publish; test spec scenarios 3748, 3749, 3760, 3761
and the publish part of 3762. Every case builds a disposable git repository with
an annotated scratch tag, fake bundles (scripts/fixtures/release/make-bundle.py)
and fake docker, gh and uname on PATH. The real scripts/release-gates.sh,
scripts/release-smoke.sh, scripts/test-portable-tls.py, scripts/release-publish.sh
and Makefile run unchanged. Nothing touches the real repository, its tags, a
real database or GitHub: gh only records its arguments, and every refusal is
checked to have printed no gh invocation and to have run no gh at all.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'scripts/fixtures/release'
TAG = 'v1.2.0'
PLATFORMS = ('macos-arm64', 'linux-x86_64')
GIT = ['git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid']


class ReleaseFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='planar-release-publish-')
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name).resolve()
        self.repo, self.tools, self.inputs = base / 'repo', base / 'tools', base / 'in'
        self.log, self.gh_log = base / 'calls.log', base / 'gh.log'
        (self.repo / 'scripts/fixtures').mkdir(parents=True)
        (self.repo / 'docker').mkdir()
        for name in ('release-publish.sh', 'release-gates.sh', 'release-smoke.sh',
                     'test-portable-tls.py', 'get-planar.sh'):
            shutil.copy2(ROOT / 'scripts' / name, self.repo / 'scripts' / name)
        shutil.copytree(ROOT / 'scripts/fixtures/portable-tls', self.repo / 'scripts/fixtures/portable-tls')
        shutil.copy2(FIXTURES / 'dist.sh', self.repo / 'scripts/dist.sh')
        shutil.copy2(ROOT / 'Makefile', self.repo / 'Makefile')
        shutil.copy2(ROOT / 'docker/linux-gate.Dockerfile', self.repo / 'docker/linux-gate.Dockerfile')
        (self.repo / '.gitignore').write_text('/build/\n/dist/\n')
        shutil.copytree(FIXTURES, self.tools)
        (base / 'home').mkdir()
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(('GIT_', 'PLANAR_', 'FAKE_', 'MAKE')) and k != 'MFLAGS'}
        self.env.update(HOME=str(base / 'home'), PATH=str(self.tools) + os.pathsep + os.environ['PATH'],
                        FAKE_LOG=str(self.log), FAKE_GH_LOG=str(self.gh_log), FAKE_TOOLS=str(self.tools),
                        PYTHONDONTWRITEBYTECODE='1')
        self.git('init', '-q')
        self.git('add', '.')
        self.git('commit', '-qm', 'release fixture source')
        self.git('tag', '-a', TAG, '-m', 'Release notes for the fixture')
        self.sha = self.git('rev-parse', 'HEAD').strip()
        self.stage = self.repo / 'dist/release' / TAG

    def git(self, *args):
        return subprocess.check_output([*GIT, *args], cwd=self.repo, env=getattr(self, 'env', None), text=True)

    def run_cmd(self, args, **updates):
        return subprocess.run(args, cwd=self.repo, env=dict(self.env, **updates),
                              capture_output=True, text=True, timeout=300)

    def make_bundle(self, platform, directory=None, version=TAG, **knobs):
        directory = directory or self.inputs / platform
        env = {f'FAKE_{k}_{platform.replace("-", "_")}': str(v) for k, v in knobs.items()}
        result = self.run_cmd(['python3', str(self.tools / 'make-bundle.py'), str(directory), platform,
                               version, self.sha, str(self.repo / 'scripts/get-planar.sh')], **env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return directory

    def gate(self, platform, directory=None, **updates):
        directory = directory or self.inputs / platform
        return self.run_cmd(['scripts/release-gates.sh', '--platform', platform, str(directory)], **updates)

    def gated(self, platform, directory=None, **knobs):
        directory = self.make_bundle(platform, directory, **knobs)
        result = self.gate(platform, directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return directory

    def ready(self):
        return [self.gated(platform) for platform in PLATFORMS]

    def evidence(self, platform, directory=None):
        return (directory or self.inputs / platform) / f'planar-{platform}.tar.gz.gates.json'

    def edit_evidence(self, platform, change):
        path = self.evidence(platform)
        record = json.loads(path.read_text())
        change(record)
        path.write_text(json.dumps(record, indent=2) + '\n')

    def publish(self, *args, dry_run=False, tag=TAG, **updates):
        dirs = [str(d) for d in (args or [self.inputs / p for p in PLATFORMS])]
        return self.run_cmd(['scripts/release-publish.sh', *(['--dry-run'] if dry_run else []), tag, *dirs],
                            **updates)

    def assert_no_publish(self, result):
        self.assertNotIn('gh release create', result.stdout)
        self.assertFalse(self.gh_log.exists(), 'gh must not run on a refusal')
        self.assertFalse(self.stage.exists(), 'a refusal stages nothing')

    def assert_refused(self, message, *args, tag=TAG, **updates):
        """Refused identically with and without --dry-run, naming the subject."""
        for dry_run in (True, False):
            result = self.publish(*args, dry_run=dry_run, tag=tag, **updates)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(message, result.stderr)
            self.assert_no_publish(result)
        return result

    def expected_assets(self, stage):
        return [stage / 'planar-linux-x86_64.tar.gz', stage / 'planar-macos-arm64.tar.gz',
                stage / 'SHA256SUMS', stage / 'VERSION', stage / 'get-planar.sh']

    def bundled_bootstrap(self, archive):
        with tarfile.open(archive) as tar:
            return tar.extractfile(archive.name[:-len('.tar.gz')] + '/get-planar.sh').read()


class GateTests(ReleaseFixture):
    """The gate runner binds smoke and CA evidence to the exact archive."""

    def test_gates_record_platform_identity_and_actual_counts(self):
        self.ready()
        for platform in PLATFORMS:
            archive = self.inputs / platform / f'planar-{platform}.tar.gz'
            record = json.loads(self.evidence(platform).read_text())
            self.assertEqual(record['format_version'], 2)
            self.assertEqual((record['archive'], record['platform']), (archive.name, platform))
            self.assertEqual(record['sha256'], hashlib.sha256(archive.read_bytes()).hexdigest())
            self.assertEqual((record['release']['sha'], record['release']['version'],
                              record['release']['schema_version']), (self.sha, TAG, 41))
            gates = record['gates']
            self.assertEqual(gates['portable'], dict(result='pass', matched_count=2, staged_binaries=5))
            self.assertEqual((gates['smoke']['result'], gates['smoke']['matched_count'],
                              gates['smoke']['expected_count']), ('pass', 7, 7))
            log = (self.inputs / platform / gates['smoke']['log']).read_text()
            for check in ('version-planar', 'version-planar-agent', 'version-planar-watch', 'version-planar-ext',
                          'help-planar-execute', 'init', 'health'):
                self.assertIn(f'smoke_check={check} result=pass', log)
            if platform == 'linux-x86_64':
                self.assertEqual({k: (v['result'], v['matched_count'], v['expected_count'])
                                  for k, v in gates.items() if k.startswith('ca_')},
                                 dict(ca_debian=('pass', 2, 2), ca_redhat=('pass', 1, 1)))
            else:
                self.assertEqual(set(gates), {'portable', 'smoke'})
        calls = [json.loads(line[len('docker '):]) for line in self.log.read_text().splitlines()]
        smoke = [c for c in calls if '/opt/planar-gates/release-smoke.sh' in c]
        self.assertEqual(len(smoke), 1, 'only the Linux smoke runs in a container')
        self.assertIn('debian:bookworm-slim', smoke[0])
        for flag, value in (('--platform', 'linux/amd64'), ('--network', 'none')):
            self.assertEqual(smoke[0][smoke[0].index(flag) + 1], value)
        clients = [c for c in calls if '/fixture/client.sh' in c]
        self.assertEqual(len(clients), 3)
        for call in clients:
            self.assertEqual(call[call.index('--platform') + 1], 'linux/amd64')
        self.assertTrue(any(c[0] == 'build' and 'dist-toolchain' in c for c in calls))

    def test_failed_smoke_is_recorded_and_refused(self):
        for platform in PLATFORMS:
            with self.subTest(platform=platform):
                self.make_bundle(platform, HEALTH_BROKEN=1)
                result = self.gate(platform)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('failed gates: smoke', result.stderr)
                smoke = json.loads(self.evidence(platform).read_text())['gates']['smoke']
                self.assertEqual((smoke['result'], smoke['matched_count'], smoke['expected_count']), ('fail', 6, 7))
                self.assertIn('smoke_check=health result=fail', result.stdout)
        self.assert_refused('smoke gate result is fail')

    def test_binaries_that_disagree_with_release_json_fail_smoke(self):
        self.make_bundle('macos-arm64', BINARY_SHA='d' * 40)
        result = self.gate('macos-arm64')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        smoke = json.loads(self.evidence('macos-arm64').read_text())['gates']['smoke']
        self.assertEqual((smoke['result'], smoke['matched_count']), ('fail', 3))
        self.assertIn('smoke_check=version-planar-ext result=fail', result.stdout)

    def test_failed_ca_trust_checks_are_recorded_and_refused(self):
        self.gated('macos-arm64')
        cases = (('FAKE_CA_FAIL', 'debian', 'ca_debian', 1), ('FAKE_CA_FAIL', 'removed', 'ca_debian', 1),
                 ('FAKE_CA_FAIL', 'redhat', 'ca_redhat', 0), ('FAKE_CA_NO_REJECTION', '1', 'ca_debian', 1))
        for variable, value, gate, matched in cases:
            with self.subTest(case=f'{variable}={value}'):
                self.make_bundle('linux-x86_64')
                result = self.gate('linux-x86_64', **{variable: value})
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f'failed gates: {gate}', result.stderr)
                record = json.loads(self.evidence('linux-x86_64').read_text())['gates'][gate]
                self.assertEqual((record['result'], record['matched_count']), ('fail', matched))
                self.assert_refused(f'{gate} gate result is fail')

    def test_toolchain_image_failure_fails_both_ca_gates(self):
        self.make_bundle('linux-x86_64')
        result = self.gate('linux-x86_64', FAKE_TOOLCHAIN_BUILD_EXIT='1')
        self.assertEqual(result.returncode, 1)
        self.assertIn('failed gates: ca_debian, ca_redhat', result.stderr)

    def test_zero_matched_portable_tests_refuse_before_gates_run(self):
        self.make_bundle('linux-x86_64', PORTABLE_MATCHED=0)
        result = self.gate('linux-x86_64')
        self.assertEqual(result.returncode, 1)
        self.assertIn('zero matched is no gate', result.stderr)
        self.assertFalse(self.log.exists(), 'no container may start for unportable evidence')
        self.assertEqual(json.loads(self.evidence('linux-x86_64').read_text())['format_version'], 1)

    def test_failed_portable_gate_refuses_before_gates_run(self):
        # Scenario 3760: an injected portability failure with a nonzero count.
        for platform in PLATFORMS:
            with self.subTest(platform=platform):
                directory = self.make_bundle(platform)
                self.edit_evidence(platform, lambda r: r['gates']['portable'].update(result='fail', matched_count=2))
                result = self.gate(platform)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f'planar-{platform}.tar.gz: the portable gate did not pass', result.stderr)
                self.assertFalse(self.log.exists(), 'no container may start for a failed portable gate')
                self.assertFalse((directory / f'planar-{platform}.gate-logs/smoke.log').exists(),
                                 'no smoke may run for a failed portable gate')
                self.assertEqual(json.loads(self.evidence(platform).read_text())['format_version'], 1)

    def test_archive_changed_after_assembly_refuses_before_gates_run(self):
        directory = self.make_bundle('linux-x86_64')
        with (directory / 'planar-linux-x86_64.tar.gz').open('ab') as archive:
            archive.write(b'\0')
        result = self.gate('linux-x86_64')
        self.assertEqual(result.returncode, 1)
        self.assertIn('the archive changed after assembly', result.stderr)
        self.assertFalse(self.log.exists())

    def test_macos_gates_need_a_macos_arm64_host(self):
        self.make_bundle('macos-arm64')
        result = self.gate('macos-arm64', FAKE_UNAME='Linux x86_64')
        self.assertEqual(result.returncode, 1)
        self.assertIn('macos-arm64 gates run on a macOS arm64 host (this host is Linux-x86_64)', result.stderr)
        self.assertEqual(json.loads(self.evidence('macos-arm64').read_text())['format_version'], 1)


class PublishTests(ReleaseFixture):
    """Scenarios 3748, 3749, 3760, 3761 and the publish part of 3762."""

    def test_dry_run_prints_the_five_asset_set_and_runs_nothing(self):
        self.ready()
        result = self.publish(dry_run=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        sums = lines[lines.index('--- SHA256SUMS') + 1:lines.index('--- VERSION')]
        archives = {p: self.inputs / p / f'planar-{p}.tar.gz' for p in PLATFORMS}
        expected = sorted([f'{hashlib.sha256(a.read_bytes()).hexdigest()}  {a.name}' for a in archives.values()] +
                          [f'{hashlib.sha256((self.repo / "scripts/get-planar.sh").read_bytes()).hexdigest()}'
                           '  get-planar.sh'], key=lambda line: line.split('  ')[1])
        self.assertEqual(sums, expected)
        self.assertEqual(lines[lines.index('--- VERSION') + 1], TAG)
        command = lines[lines.index('--- command') + 1]
        self.assertEqual(shlex.split(command),
                         ['gh', 'release', 'create', TAG, '--verify-tag', '--title', TAG, '--notes-from-tag',
                          *map(str, self.expected_assets(self.stage))])
        self.assertIn('linux-x86_64 planar-linux-x86_64.tar.gz gates: portable=2 smoke=7/7 ca_debian=2/2 '
                      'ca_redhat=1/1', result.stdout)
        self.assertIn('macos-arm64 planar-macos-arm64.tar.gz gates: portable=2 smoke=7/7', result.stdout)
        self.assertFalse(self.gh_log.exists(), 'a dry run runs nothing')
        self.assertFalse((self.repo / 'dist').exists(), 'a dry run stages nothing')

    def test_publish_uploads_verified_assets_with_one_bootstrap(self):
        self.ready()
        result = self.publish()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line) for line in self.gh_log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        assets = self.expected_assets(self.stage)
        self.assertEqual(calls[0]['args'], ['release', 'create', TAG, '--verify-tag', '--title', TAG,
                                            '--notes-from-tag', *map(str, assets)])
        self.assertEqual(Path(calls[0]['cwd']).resolve(), self.repo)
        self.assertEqual(set(calls[0]['files']), set(map(str, assets)), 'every asset existed when gh ran')
        self.assertEqual(sorted(p.name for p in self.stage.iterdir()), sorted(a.name for a in assets))
        for platform in PLATFORMS:
            name = f'planar-{platform}.tar.gz'
            self.assertEqual((self.stage / name).read_bytes(), (self.inputs / platform / name).read_bytes())
        # The published bootstrap is byte-identical to both bundle copies and the tagged source.
        published = (self.stage / 'get-planar.sh').read_bytes()
        self.assertEqual(published, self.git('show', f'{TAG}:scripts/get-planar.sh').encode())
        for platform in PLATFORMS:
            self.assertEqual(self.bundled_bootstrap(self.stage / f'planar-{platform}.tar.gz'), published)
        self.assertTrue(os.access(self.stage / 'get-planar.sh', os.X_OK))
        records = (self.stage / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(records), 3)
        for record in records:
            digest, name = record.split('  ')
            self.assertEqual(hashlib.sha256((self.stage / name).read_bytes()).hexdigest(), digest)
        self.assertEqual((self.stage / 'VERSION').read_text(), TAG + '\n')
        self.assertEqual(self.git('tag', '-l'), TAG + '\n', 'the publisher never creates a tag')

    def test_failed_gh_reports_and_keeps_the_stage(self):
        self.ready()
        result = self.publish(FAKE_GH_EXIT='1')
        self.assertEqual(result.returncode, 1)
        self.assertIn('gh release create failed', result.stderr)
        self.assertTrue((self.stage / 'SHA256SUMS').exists())

    def test_version_mismatch_is_refused_naming_bundle_and_versions(self):
        self.gated('macos-arm64')
        self.gated('linux-x86_64', VERSION='v1.1.0')
        result = self.assert_refused('bundle version v1.1.0 differs from release tag v1.2.0')
        self.assertIn(str(self.inputs / 'linux-x86_64/planar-linux-x86_64.tar.gz'), result.stderr)

    def test_pre_release_and_malformed_tags_are_refused(self):
        self.ready()
        self.git('tag', '-a', 'v1.2.0-rc1', '-m', 'candidate')
        self.assert_refused('pre-release tag v1.2.0-rc1 refused', tag='v1.2.0-rc1')
        self.assert_refused('is not a stable vMAJOR.MINOR.PATCH tag', tag='v01.2.0')

    def test_missing_tag_is_refused_and_never_created(self):
        self.ready()
        self.assert_refused('missing release tag v9.9.9', tag='v9.9.9')
        self.assertEqual(self.git('tag', '-l', 'v9.9.9'), '')

    def test_lightweight_tag_is_refused(self):
        self.ready()
        self.git('tag', '-d', TAG)
        self.git('tag', TAG)
        self.assert_refused('is lightweight')

    def test_head_at_another_commit_is_refused(self):
        self.ready()
        self.git('commit', '--allow-empty', '-qm', 'after the tag')
        self.assert_refused(f'but release tag {TAG} is at {self.sha}')

    def test_mixed_and_foreign_commits_are_refused(self):
        self.gated('macos-arm64')
        self.gated('linux-x86_64', SHA='f' * 40)
        self.assert_refused(f'mixed bundle commits: linux-x86_64 at {"f" * 40}, macos-arm64 at {self.sha}')
        for platform in PLATFORMS:
            self.gated(platform, SHA='e' * 40)
        self.assert_refused(f'bundles were built from {"e" * 40} but release tag {TAG} is at {self.sha}')

    def test_mixed_schemas_are_refused(self):
        self.gated('macos-arm64')
        self.gated('linux-x86_64', SCHEMA=42)
        self.assert_refused('mixed bundle schemas: linux-x86_64 schema 42, macos-arm64 schema 41')

    def test_missing_and_duplicate_platforms_are_refused(self):
        self.ready()
        self.assert_refused('missing platform bundle: linux-x86_64', self.inputs / 'macos-arm64')
        second = self.gated('linux-x86_64', self.inputs / 'second')
        self.assert_refused('duplicate linux-x86_64 bundles', *[self.inputs / p for p in PLATFORMS], second)

    def test_wrong_platform_floor_is_refused(self):
        self.gated('macos-arm64', FLOOR='15.0')
        self.gated('linux-x86_64')
        self.assert_refused('os_floor 15.0 is not the macos-arm64 floor 26.0')

    def test_missing_or_assembly_only_evidence_is_refused(self):
        self.ready()
        self.make_bundle('linux-x86_64')  # a rebuild writes assembly evidence only
        self.assert_refused('format_version 1 is not release evidence')
        self.evidence('linux-x86_64').unlink()
        self.assert_refused('missing gate evidence planar-linux-x86_64.tar.gz.gates.json')

    def test_missing_gate_records_are_refused(self):
        for platform, gate in (('linux-x86_64', 'ca_redhat'), ('linux-x86_64', 'ca_debian'),
                               ('macos-arm64', 'smoke'), ('macos-arm64', 'portable')):
            with self.subTest(gate=gate):
                self.ready()
                self.edit_evidence(platform, lambda r, g=gate: r['gates'].pop(g))
                self.assert_refused(f'missing {gate} gate evidence')

    def test_zero_or_partial_matched_counts_are_refused(self):
        changes = (('portable', dict(matched_count=0), 'portable gate matched 0 checks; zero matched is no gate'),
                   ('smoke', dict(matched_count=0, expected_count=0), 'smoke gate matched 0 checks'),
                   ('smoke', dict(matched_count=6), 'smoke gate matched 6 of 7 expected checks'),
                   ('portable', dict(staged_binaries=4), 'inspected 4 staged binaries, not 5'))
        for gate, fields, message in changes:
            with self.subTest(message=message):
                self.ready()
                self.edit_evidence('macos-arm64', lambda r, g=gate, f=fields: r['gates'][g].update(f))
                self.assert_refused(message)

    def test_evidence_short_of_a_complete_gate_is_refused(self):
        # A record that says pass but ran fewer checks than a complete gate:
        # the publisher owns each gate's size and the CA gates' case lists.
        def drop_expected(fields):
            return lambda gate: (gate.pop('expected_count'), gate.update(fields))
        changes = (
            ('macos-arm64', 'smoke', drop_expected(dict(matched_count=1)),
             'macos-arm64 smoke gate matched 1 checks and records no expected_count; missing checks'),
            ('linux-x86_64', 'ca_redhat', drop_expected({}),
             'linux-x86_64 ca_redhat gate matched 1 checks and records no expected_count; missing checks'),
            ('linux-x86_64', 'ca_debian', lambda g: g.update(matched_count=1, expected_count=1, cases=['debian']),
             'linux-x86_64 ca_debian gate matched 1 of 1 expected checks, below the 2 a complete '
             'linux-x86_64 ca_debian gate runs; missing checks'),
            ('macos-arm64', 'portable', lambda g: g.update(matched_count=1),
             'macos-arm64 portable gate matched 1 of no recorded expected checks, below the 2 a complete '
             'macos-arm64 portable gate runs; missing checks'),
            ('linux-x86_64', 'portable', lambda g: g.update(matched_count=1),
             'linux-x86_64 portable gate matched 1 of no recorded expected checks, below the 2'),
            ('linux-x86_64', 'smoke', lambda g: g.update(matched_count=6, expected_count=6),
             'linux-x86_64 smoke gate matched 6 of 6 expected checks, below the 7'),
            ('linux-x86_64', 'ca_debian', lambda g: g.update(cases=['debian', 'debian']),
             "linux-x86_64 ca_debian gate ran cases ['debian', 'debian'], not ['debian', 'removed'] "
             '(matched 2 of 2 expected checks); missing checks'),
            ('linux-x86_64', 'ca_redhat', lambda g: g.pop('cases'),
             "linux-x86_64 ca_redhat gate ran cases None, not ['redhat']"),
            ('macos-arm64', 'smoke', lambda g: g.update(expected_count='7'),
             "macos-arm64 smoke gate expected_count '7' is not a count"),
            ('macos-arm64', 'smoke', lambda g: g.update(expected_count=8),
             'planar-macos-arm64.tar.gz: macos-arm64 smoke gate matched 7 of 8 expected checks'),
        )
        for platform, gate, change, message in changes:
            with self.subTest(message=message):
                self.ready()
                self.edit_evidence(platform, lambda r, g=gate, c=change: c(r['gates'][g]))
                self.assert_refused(message)

    def test_failed_portable_gate_in_release_evidence_is_refused(self):
        for platform in PLATFORMS:
            with self.subTest(platform=platform):
                self.ready()
                self.edit_evidence(platform, lambda r: r['gates']['portable'].update(result='fail'))
                self.assert_refused(f'planar-{platform}.tar.gz: portable gate result is fail, not pass')

    def test_archive_changed_after_checks_invalidates_its_evidence(self):
        self.ready()
        with (self.inputs / 'macos-arm64/planar-macos-arm64.tar.gz').open('ab') as archive:
            archive.write(b'\0')
        self.assert_refused('the archive changed after its gates ran')

    def test_evidence_for_another_identity_is_refused(self):
        self.ready()
        self.edit_evidence('linux-x86_64', lambda r: r['release'].update(schema_version=40))
        self.assert_refused('evidence release identity differs')

    def test_bootstrap_copies_must_match_the_tagged_source(self):
        self.gated('macos-arm64', BOOTSTRAP_EXTRA='# drift\n')
        self.gated('linux-x86_64')
        self.assert_refused('bundled get-planar.sh differs from v1.2.0:scripts/get-planar.sh')
        self.gated('macos-arm64')
        self.gated('linux-x86_64', STANDALONE_EXTRA='# drift\n')
        self.assert_refused('get-planar.sh differs from v1.2.0:scripts/get-planar.sh')


class ReleaseCutTests(ReleaseFixture):
    """make release-cut sequences assembly, both platforms' gates and the publisher."""

    def cut(self, *extra, **updates):
        return self.run_cmd(['make', '--no-print-directory', 'release-cut', f'TAG={TAG}', *extra], **updates)

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_dry_run_cut_gates_both_platforms_and_prints_the_command(self):
        result = self.cut('DRY_RUN=1')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('gh release create v1.2.0 --verify-tag', result.stdout)
        self.assertFalse(self.gh_log.exists())
        self.assertIn(f'fake-dist {TAG}', self.calls())
        out = self.repo / 'build/release-cut' / TAG
        for platform in PLATFORMS:
            record = json.loads(self.evidence(platform, out / platform).read_text())
            self.assertEqual((record['format_version'], record['release']['sha']), (2, self.sha))
            self.assertEqual(record['gates']['smoke']['matched_count'], 7)

    def test_cut_publishes_once_with_five_assets(self):
        result = self.cut()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line) for line in self.gh_log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]['args'][:8], ['release', 'create', TAG, '--verify-tag', '--title', TAG,
                                                '--notes-from-tag', str(self.stage / 'planar-linux-x86_64.tar.gz')])
        self.assertEqual(len(calls[0]['files']), 5)

    def test_failing_gates_stop_the_cut_before_publication(self):
        cases = (dict(FAKE_HEALTH_BROKEN_linux_x86_64='1'), dict(FAKE_HEALTH_BROKEN_macos_arm64='1'),
                 dict(FAKE_CA_FAIL='redhat'), dict(FAKE_PORTABLE_MATCHED_macos_arm64='0'),
                 dict(FAKE_PORTABLE_MATCHED_linux_x86_64='0'))
        for updates in cases:
            with self.subTest(case=updates):
                result = self.cut(**updates)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('release-gates:', result.stderr)
                self.assert_no_publish(result)

    def test_dry_run_accepts_only_one(self):
        for value in ('0', 'yes', 'true', '2'):
            with self.subTest(DRY_RUN=value):
                result = self.cut(f'DRY_RUN={value}')
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(f"DRY_RUN must be 1 (a dry run) or unset (publish), not '{value}'", result.stderr)
                self.assert_no_publish(result)
                self.assertEqual(self.calls(), [], 'no build or container may start')
                self.assertFalse((self.repo / 'build/release-cut').exists())

    def test_identity_refusals_stop_before_any_build(self):
        result = self.run_cmd(['make', 'release-cut', 'TAG=v9.9.9'])
        self.assertIn('missing release tag v9.9.9', result.stderr)
        self.git('commit', '--allow-empty', '-qm', 'after the tag')
        moved = self.cut()
        self.assertIn('HEAD is at', moved.stderr)
        self.git('reset', '-q', '--hard', TAG)
        (self.repo / 'untracked-source').write_text('dirt')
        dirty = self.cut()
        self.assertIn('dirty source', dirty.stderr)
        (self.repo / 'untracked-source').unlink()
        host = self.cut(FAKE_UNAME='Linux x86_64')
        self.assertIn('needs a macOS arm64 host (this host is Linux-x86_64)', host.stderr)
        no_tag = self.run_cmd(['make', 'release-cut'])
        self.assertIn('TAG=vMAJOR.MINOR.PATCH is required', no_tag.stderr)
        pre = self.run_cmd(['make', 'release-cut', 'TAG=v1.2.0-rc1'])
        self.assertIn('pre-release tag v1.2.0-rc1 refused', pre.stderr)
        for refused in (result, moved, dirty, host, no_tag, pre):
            self.assertNotEqual(refused.returncode, 0)
            self.assert_no_publish(refused)
        self.assertEqual(self.calls(), [], 'no build or container may start')


if __name__ == '__main__':
    unittest.main(verbosity=2)
