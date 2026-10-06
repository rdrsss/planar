#!/usr/bin/env python3
"""Exercise real dist assembly with fake products, never native portability."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BINARIES = {'planar', 'planar-agent', 'planar-watch', 'planar-execute', 'planar-ext'}
FIELDS = {'version', 'sha', 'date', 'os', 'arch', 'os_floor', 'schema_version'}
FUTURE_SCRIPTS = ('get-planar.sh', 'uninstall.sh')
spec = importlib.util.spec_from_file_location('dist_identity', ROOT / 'scripts/dist-identity.test.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_checksums(directory, asset):
    """Check the actual producer record with both host tools when available."""
    require(Path(asset).name == asset, 'checksum asset must be a basename')
    lines = (directory / 'SHA256SUMS').read_text().splitlines()
    matches = [line for line in lines if line.endswith('  ' + asset)]
    require(len(matches) == 1, f'{asset}: expected exactly one checksum record')
    record = matches[0]
    require(re.fullmatch(r'[0-9a-f]{64}  ' + re.escape(asset), record),
            f'{asset}: malformed checksum record')
    tools = [args for args in (['sha256sum', '-c', '-'], ['shasum', '-a', '256', '-c', '-'])
             if shutil.which(args[0])]
    require(tools, f'{asset}: no SHA-256 checker available')
    for args in tools:
        result = subprocess.run(args, cwd=directory, input=record + '\n', text=True,
                                capture_output=True, timeout=30)
        require(result.returncode == 0,
                f'{asset}: {args[0]} checksum failed: {result.stdout}{result.stderr}')
        print(f'checksum: {asset}: {args[0]} verified', flush=True)


def check_layout(stage, source, fake_bin, expected):
    entries = {'release.json', 'install.sh', 'install-cleanup.txt', 'bin', 'skills',
               'agents', 'codex-agents', 'templates', 'workflows', 'migrations', 'scripts'}
    entries.update(name for name in FUTURE_SCRIPTS if (source / name).is_file())
    require({p.name for p in stage.iterdir()} == entries, 'top-level entries differ')
    for name in ('install.sh', 'install-cleanup.txt', *FUTURE_SCRIPTS):
        if name in entries:
            require((stage / name).read_bytes() == (source / name).read_bytes(),
                    f'{name}: not byte-identical')
    require({p.name for p in (stage / 'bin').iterdir()} == BINARIES, 'bin entries differ')
    for name in BINARIES:
        product = stage / 'bin' / name
        require(product.is_file() and not product.is_symlink() and os.access(product, os.X_OK),
                f'{name}: not a regular executable')
        require(product.read_bytes() == (fake_bin / name).read_bytes(), f'{name}: bytes differ')
    require({p.name for p in (stage / 'scripts').iterdir()} == {'install-lib'},
            'scripts must contain install-lib only')
    require({p.name for p in (stage / 'skills').iterdir()} == {'planar'}, 'skills entries differ')
    # Verify authored content, including nested files, rather than directory presence alone.
    for name in ('skills/planar', 'agents', 'templates', 'workflows', 'migrations', 'scripts/install-lib'):
        def snapshot(directory):
            return {p.relative_to(directory).as_posix():
                    ('link', os.readlink(p)) if p.is_symlink() else
                    ('file', p.read_bytes()) if p.is_file() else ('directory', None)
                    for p in directory.rglob('*')
                    if not any(part == '__pycache__' or part == '.DS_Store' or part.endswith('.pyc')
                               for part in p.relative_to(directory).parts)}
        require(snapshot(stage / name) == snapshot(source / name), f'{name}: authored content differs')
    authored = {p.stem for p in (source / 'agents').glob('planar-*.md')}
    require(authored, 'no authored agents')
    require({p.name for p in (stage / 'codex-agents').iterdir()} ==
            {name + '.toml' for name in authored}, 'codex-agents entries differ')
    for name in authored:
        rendered = stage / 'codex-agents' / (name + '.toml')
        require(rendered.is_file() and not rendered.is_symlink() and rendered.stat().st_size > 0,
                f'{name}: missing rendered agent')
        require(rendered.read_text().startswith(f'name = "{name}"\n'), f'{name}: wrong rendered agent')
    text = (stage / 'release.json').read_text()
    metadata = json.loads(text)
    require(isinstance(metadata, dict) and set(metadata) == FIELDS, 'release.json fields differ')
    lines = text.splitlines()
    require(lines[0] == '{' and lines[-1] == '}' and len(lines) == len(FIELDS) + 2,
            'release.json must have one key per line')
    keys = []
    for line in lines[1:-1]:
        match = re.fullmatch(r'  "([a-z_]+)": ("[^"\n]*"|[0-9]+),?', line)
        require(match is not None, 'release.json must be flat with one pair per line')
        keys.append(match[1])
    require(set(keys) == FIELDS and len(set(keys)) == len(keys), 'release.json duplicate keys')
    require(metadata == expected, f'release.json values differ: {metadata!r}')
    require(not isinstance(metadata['schema_version'], bool), 'schema_version cannot be boolean')


class LayoutTests(unittest.TestCase):
    def setUp(self):
        # Share external-command fixtures, not assembly or its layout implementation.
        self.fixture = identity.IdentityTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.root = self.fixture.root
        self.fixture.prepare_commands()
        self.fake_bin = self.root / 'build/fake-bin'
        self.fake_bin.mkdir()
        for name in BINARIES:
            shutil.copy2(self.root / 'build/fixture-tools/fixture-binary', self.fake_bin / name)
        require({p.name for p in self.fake_bin.iterdir()} == BINARIES, 'fake bin must have five products')
        self.schema = max(int(p.name.split('_', 1)[0]) for p in (self.root / 'migrations').glob('*.up.sql'))
        self.fixture.env['HEALTH_JSON'] = json.dumps(dict(schema_current=True,
                                                        schema_version=self.schema, schema_target=self.schema))
        # Configure/install stand-ins consume the fake bin; real dist.sh copies all assets,
        # invokes the real renderer, collects metadata and emits the archive/checksums.
        (self.root / 'build/fixture-tools/cmake').write_text('''#!/usr/bin/env python3
import pathlib, shutil, sys
root = pathlib.Path.cwd()
if '--preset' in sys.argv:
    (root / 'build/dist').mkdir(exist_ok=True)
    (root / 'build/dist/CMakeCache.txt').write_text('PLANAR_LLVM_PREFIX:PATH=/fixture/toolchain\\n')
if '--install' in sys.argv:
    target = pathlib.Path(sys.argv[sys.argv.index('--prefix') + 1]) / 'bin'
    shutil.copytree(root / 'build/fake-bin', target)
''')
        for name in FUTURE_SCRIPTS:
            if (ROOT / name).is_file():
                shutil.copy2(ROOT / name, self.root / name)

    def assemble(self, version='dev'):
        updates = {} if version == 'dev' else {'PLANAR_RELEASE_VERSION': version}
        result = self.fixture.run_dist(**updates)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        assets = list((self.root / 'dist').glob('planar-*.tar.gz'))
        self.assertEqual(len(assets), 1)
        archive = assets[0]
        system = os.uname()
        platform = {('Darwin', 'arm64'): ('macos', 'arm64', '26.0'),
                    ('Linux', 'x86_64'): ('linux', 'x86_64', '2.36')}[(system.sysname, system.machine)]
        self.assertEqual(archive.name, f'planar-{platform[0]}-{platform[1]}.tar.gz')
        check_checksums(archive.parent, archive.name)
        self.assertEqual((archive.parent / 'VERSION').read_text(), version + '\n')
        extract = self.root / 'build/unpacked'
        shutil.rmtree(extract, ignore_errors=True)
        extract.mkdir()
        with tarfile.open(archive) as tar:
            members = tar.getmembers()
            names = [m.name for m in members]
            self.assertEqual(names, sorted(set(names)))
            bundle = archive.name.removesuffix('.tar.gz')
            self.assertTrue(all(name == bundle or name.startswith(bundle + '/') for name in names))
            self.assertTrue(all('..' not in Path(name).parts and not Path(name).is_absolute() for name in names))
            self.assertTrue(all((m.uid, m.gid, m.uname, m.gname) == (0, 0, 'root', 'root') for m in members))
            tar.extractall(extract, filter='data')
        stage = extract / bundle
        expected = dict(version=version, sha=self.fixture.sha, date='2026-10-06T00:00:00Z',
                        os=platform[0], arch=platform[1], os_floor=platform[2], schema_version=self.schema)
        check_layout(stage, self.root, self.fake_bin, expected)
        return archive, stage, expected, names

    def test_current_tree_layout_and_checksums(self):
        self.assemble()

    def test_later_milestone_scripts_are_conditional_byte_copies(self):
        for name in FUTURE_SCRIPTS:
            (self.root / name).unlink(missing_ok=True)
        self.assemble()
        for present in ((FUTURE_SCRIPTS[0],), (FUTURE_SCRIPTS[1],), FUTURE_SCRIPTS):
            with self.subTest(scripts=present):
                for name in FUTURE_SCRIPTS:
                    (self.root / name).unlink(missing_ok=True)
                for name in present:
                    (self.root / name).write_text(f'#!/bin/bash\n# future fixture {name}\nexit 0\n')
                self.assemble()

    def test_two_tagged_cuts_retain_identity_layout_and_owners(self):
        self.fixture.git('add', '.')
        self.fixture.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                         'commit', '--allow-empty', '-qm', 'layout snapshot')
        self.fixture.sha = self.fixture.git('rev-parse', 'HEAD').strip()
        self.fixture.git('tag', 'v1.2.4')
        first = self.assemble('v1.2.4')
        second = self.assemble('v1.2.4')
        self.assertEqual(first[2:], second[2:])

    def test_layout_assertions_reject_deliberate_mutations(self):
        _, stage, expected, _ = self.assemble()
        mutations = [('top-level entries', stage / 'extra', b'extra'),
                     ('bin entries', stage / 'bin/extra', b'extra'),
                     ('scripts must contain', stage / 'scripts/extra', b'extra'),
                     ('codex-agents entries', stage / 'codex-agents/extra.toml', b'extra'),
                     ('not byte-identical', stage / 'install.sh', b'wrong installer'),
                     ('release.json fields', stage / 'release.json', b'{}\n'),
                     ('one key per line', stage / 'release.json', json.dumps(expected).encode()),
                     ('values differ', stage / 'release.json',
                      (json.dumps(dict(expected, schema_version=self.schema + 1), indent=2) + '\n').encode())]
        for message, path, replacement in mutations:
            with self.subTest(assertion=message):
                original = path.read_bytes() if path.exists() else None
                path.write_bytes(replacement)
                with self.assertRaisesRegex(AssertionError, message):
                    check_layout(stage, self.root, self.fake_bin, expected)
                if original is None:
                    path.unlink()
                else:
                    path.write_bytes(original)
        victim = stage / 'bin/planar-watch'
        victim.unlink()
        with self.assertRaisesRegex(AssertionError, 'bin entries'):
            check_layout(stage, self.root, self.fake_bin, expected)

    def test_corrupt_archive_fails_shell_checksum_assertion(self):
        archive, _, _, _ = self.assemble()
        with archive.open('ab') as output:
            output.write(b'x')
        result = subprocess.run(['bash', str(ROOT / 'scripts/dist-test.sh'), '--check-checksums',
                                 str(archive.parent), archive.name], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(archive.name, result.stderr)
        self.assertIn('checksum failed', result.stderr)
        print('deliberate archive corruption rejected: ' + result.stderr.strip(), flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--check-checksums':
        if len(sys.argv) != 4:
            sys.exit('usage: dist-test.sh --check-checksums DIRECTORY ASSET')
        try:
            check_checksums(Path(sys.argv[2]), sys.argv[3])
        except (AssertionError, OSError) as error:
            sys.exit(f'dist-test: {error}')
    else:
        unittest.main(verbosity=2)
