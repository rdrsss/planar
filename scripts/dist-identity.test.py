#!/usr/bin/env python3
"""Controlled dist refusal tests; real binary portability is validated separately."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='planar-dist-identity-')
        self.root = Path(self.temp.name)
        for name in ('skills', 'agents', 'templates', 'workflows', 'migrations'):
            shutil.copytree(ROOT / name, self.root / name)
        (self.root / 'scripts').mkdir()
        for name in ('dist.sh', 'render-codex-agents.py'):
            shutil.copy2(ROOT / 'scripts' / name, self.root / 'scripts' / name)
        shutil.copytree(ROOT / 'scripts/install-lib', self.root / 'scripts/install-lib')
        for name in ('install.sh', 'install-cleanup.txt'):
            shutil.copy2(ROOT / name, self.root / name)
        (self.root / '.gitignore').write_text('/build/\n/dist/\n/compile_commands.json\n')
        self.env = dict(os.environ)
        for name in ('PLANAR_RELEASE_VERSION', 'PLANAR_SOURCE_SHA', 'PLANAR_SOURCE_DIRTY'):
            self.env.pop(name, None)
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'disposable dist fixture')
        self.sha = self.git('rev-parse', 'HEAD').strip()
        self.git('tag', 'v1.2.3')

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, text=True)

    def run_dist(self, identity=False, **updates):
        return subprocess.run(['bash', 'scripts/dist.sh', *(['--identity'] if identity else [])],
                              cwd=self.root, env=dict(self.env, **updates),
                              capture_output=True, text=True, timeout=60)

    def refused(self, result, message):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse(list((self.root / 'dist').glob('planar-*.tar.gz')))
        self.assertFalse(list((self.root / 'build').glob('dist-cut.*')))

    def test_tag_refusals_and_owned_outputs(self):
        for tag in ('v1.2.3-rc1', 'dev', 'v01.2.3'):
            self.refused(self.run_dist(True, PLANAR_RELEASE_VERSION=tag), 'stable')
        self.refused(self.run_dist(True, PLANAR_RELEASE_VERSION='v9.9.9'), 'missing release tag')
        (self.root / 'dist').mkdir()
        (self.root / 'dist/old.tar.gz').write_text('owned output')
        result = self.run_dist(True, PLANAR_RELEASE_VERSION='v1.2.3')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, self.sha + '\n0\n')
        (self.root / 'foreign-source').write_text('untracked source dirt')
        self.refused(self.run_dist(True, PLANAR_RELEASE_VERSION='v1.2.3'), 'dirty')
        self.assertEqual(self.run_dist(True).stdout, self.sha + '\n1\n')
        self.assertEqual((self.root / 'dist/old.tar.gz').read_text(), 'owned output')
        (self.root / 'foreign-source').unlink()
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '--allow-empty', '-qm', 'different HEAD')
        self.refused(self.run_dist(True, PLANAR_RELEASE_VERSION='v1.2.3'), 'requires HEAD')

    def test_snapshot_identity_refusals(self):
        self.refused(self.run_dist(True, PLANAR_SOURCE_SHA='0' * 40), 'differs from HEAD')
        self.refused(self.run_dist(True, PLANAR_SOURCE_DIRTY='1'), 'differs from snapshot')
        shutil.rmtree(self.root / '.git')
        self.refused(self.run_dist(True), 'full source SHA')
        self.refused(self.run_dist(True, PLANAR_SOURCE_SHA=self.sha, PLANAR_SOURCE_DIRTY='no'), 'dirty state')
        self.refused(self.run_dist(True, PLANAR_RELEASE_VERSION='v1.2.3',
                                   PLANAR_SOURCE_SHA=self.sha, PLANAR_SOURCE_DIRTY='1'), 'clean source')
        result = self.run_dist(True, PLANAR_RELEASE_VERSION='v1.2.3',
                               PLANAR_SOURCE_SHA=self.sha, PLANAR_SOURCE_DIRTY='0')
        self.assertEqual(result.returncode, 0, result.stderr)

    def prepare_commands(self):
        """Stub only external gates to reach metadata refusals, never prove portability."""
        tools = self.root / 'build/fixture-tools'
        tools.mkdir(parents=True)
        binary = tools / 'fixture-binary'
        binary.write_text('''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
assert '/dist-cut.' in os.environ['HOME']
if os.environ.get('EXPECT_ISOLATED_CWD') and sys.argv[1] in ('init', 'health'):
    assert Path.cwd() == Path(os.environ['HOME']), 'metadata cwd must be the isolated HOME'
    assert not os.environ.get('GIT_DIR') and not os.environ.get('GIT_WORK_TREE')
    Path(os.environ['ARENA_RECORD']).write_text(os.environ['HOME'])
assert os.environ['PLANAR_DB'].startswith(os.environ['HOME'] + '/')
assert os.environ['PLANAR_CONFIG_PATH'].startswith(os.environ['HOME'] + '/')
if sys.argv[1] == 'init':
    sys.exit(int(os.environ.get('INIT_EXIT', '0')))
if sys.argv[1] == 'health':
    print(os.environ.get('HEALTH_JSON', '{"schema_current":true,"schema_version":41,"schema_target":41}'))
    sys.exit(int(os.environ.get('HEALTH_EXIT', '0')))
print(json.dumps(dict(release=os.environ.get('BINARY_RELEASE', os.environ.get('PLANAR_RELEASE_VERSION') or 'dev'),
sha=os.environ.get('BINARY_SHA', os.environ['PLANAR_SOURCE_SHA']),
dirty=os.environ['PLANAR_SOURCE_DIRTY'] == '1', date='2026-10-06T00:00:00Z')))
''')
        binary.chmod(0o755)
        cmake = tools / 'cmake'
        cmake.write_text('''#!/usr/bin/env python3
import os, pathlib, shutil, sys
root = pathlib.Path.cwd()
if '--preset' in sys.argv:
    (root / 'build/dist').mkdir(exist_ok=True)
    (root / 'build/dist/CMakeCache.txt').write_text('PLANAR_LLVM_PREFIX:PATH=/fixture/toolchain\\n')
if '--install' in sys.argv:
    target = pathlib.Path(sys.argv[sys.argv.index('--prefix') + 1]) / 'bin'
    target.mkdir(parents=True)
    for name in ('planar', 'planar-agent', 'planar-watch', 'planar-ext', 'planar-execute'):
        shutil.copy2(root / 'build/fixture-tools/fixture-binary', target / name)
''')
        cmake.chmod(0o755)
        ctest = tools / 'ctest'
        ctest.write_text('''#!/usr/bin/env python3
import json, os, sys
if '--show-only=json-v1' in sys.argv:
    print(json.dumps(dict(tests=[] if os.environ.get('ZERO_TESTS') else
        [dict(name='portable.binaries'), dict(name='portable.inspector')])))
else:
    sys.exit(int(os.environ.get('PORTABLE_EXIT', '0')))
''')
        ctest.chmod(0o755)
        (self.root / 'scripts/portable-check.py').write_text('# Controlled fixture only\n')
        self.env['PATH'] = str(tools) + os.pathsep + self.env['PATH']

    def test_metadata_schema_refusals(self):
        self.prepare_commands()
        self.refused(self.run_dist(INIT_EXIT='1'), 'initialization failed')
        self.refused(self.run_dist(HEALTH_EXIT='1'), 'health failed')
        for health in ('invalid json', '{}',
                       '{"schema_current":false,"schema_version":41,"schema_target":41}',
                       '{"schema_current":true,"schema_version":40,"schema_target":41}',
                       '{"schema_current":true,"schema_version":true,"schema_target":true}',
                       '{"schema_current":true,"schema_version":41,"schema_target":41.0}'):
            self.refused(self.run_dist(HEALTH_JSON=health), 'invalid bundle metadata')
        self.refused(self.run_dist(BINARY_SHA='0' * 40), 'binary metadata differs')
        self.refused(self.run_dist(BINARY_RELEASE='v9.9.9'), 'binary metadata differs')

    def test_metadata_cwd_is_outside_source_worktree(self):
        self.prepare_commands()
        record = self.root / 'build/arena-record'
        result = self.run_dist(EXPECT_ISOLATED_CWD='1', ARENA_RECORD=str(record))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        home = Path(record.read_text())
        self.assertFalse(home.is_relative_to(self.root))
        self.assertFalse(home.exists(), 'metadata arena must be removed')

    def test_zero_or_failed_portable_checks_refuse(self):
        self.prepare_commands()
        self.refused(self.run_dist(ZERO_TESTS='1'), 'expected both portable tests')
        self.refused(self.run_dist(PORTABLE_EXIT='1'), 'portable tests failed')

    def test_dev_schema_field_type_and_cleanup(self):
        self.prepare_commands()
        result = self.run_dist(HEALTH_JSON='{"schema_current":true,"schema_version":41,"schema_target":"041"}')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        import tarfile
        archive = next((self.root / 'dist').glob('*.tar.gz'))
        with tarfile.open(archive) as tar:
            metadata = json.load(tar.extractfile(archive.name[:-7] + '/release.json'))
        self.assertEqual(metadata['schema_version'], '041')
        self.assertEqual(metadata['version'], 'dev')
        self.assertFalse(list((self.root / 'build').glob('dist-cut.*')))
        evidence = json.loads(Path(str(archive) + '.gates.json').read_text())
        self.assertEqual(evidence['gates']['portable']['matched_count'], 2)
        self.assertNotIn('smoke', evidence['gates'])
        self.assertNotIn('ca', evidence['gates'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
