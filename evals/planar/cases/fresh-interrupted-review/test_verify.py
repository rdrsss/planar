import hashlib
import tempfile
import unittest
from pathlib import Path


class FixtureControls(unittest.TestCase):
    def test_actual_portable_install(self):
        from install_fixture import install
        with tempfile.TemporaryDirectory() as tmp:
            result = install(tmp)
            self.assertEqual(result['files'], 32)
            self.assertEqual((Path(tmp)/'repository/STATE.txt').read_bytes(), b'fault-injected\n')
            self.assertEqual((Path(tmp)/'repository/RESULT.txt').read_bytes(), b'2\n')
            with self.assertRaises(ValueError): install(tmp)

class SyntheticV2Controls(unittest.TestCase):
    def test_v2_is_single_documented_transform(self):
        import json
        from materialize_v2 import derive_packet
        case=Path(__file__).resolve().parent; old=case/'fixture'; new=case/'fixture-v2'
        oi=json.loads((old/'inventory.json').read_text()); ni=json.loads((new/'inventory.json').read_text())
        self.assertEqual(ni['schema'],2)
        packet='home/projects/revision-fault/tasks/T-001/reviewer-packet-1.md'
        worker='home/projects/revision-fault/tasks/T-001/worker-packet-1.md'
        self.assertEqual(derive_packet((old/packet).read_bytes(),(old/worker).read_bytes()),(new/packet).read_bytes())
        for name in oi['files']:
            if name != packet: self.assertEqual((old/name).read_bytes(),(new/name).read_bytes())
            self.assertEqual(hashlib.sha256((new/name).read_bytes()).hexdigest(),ni['files'][name]['portable_sha256'])
        bundled=case/'tests/fixture-v2'
        self.assertEqual({p.relative_to(new) for p in new.rglob('*') if p.is_file()}, {p.relative_to(bundled) for p in bundled.rglob('*') if p.is_file()})
        for path in new.rglob('*'):
            if path.is_file(): self.assertEqual(path.read_bytes(),(bundled/path.relative_to(new)).read_bytes())

class CompatibilityControls(unittest.TestCase):
    def test_current_packet_needs_replacement(self):
        from compatibility import inspect
        case = Path(__file__).resolve().parent
        root = case.parents[3]
        result = inspect(root/'plugins/planar', case/'fixture')
        self.assertEqual(result['missing_packet_sections'], ['## Checks this task must pass'])
        self.assertTrue(result['immutable_replacement_required'])
        clean = inspect(root/'plugins/planar', case/'fixture-v2')
        self.assertEqual(clean['missing_packet_sections'], [])
        self.assertFalse(clean['immutable_replacement_required'])

class HistoricalSourceControls(unittest.TestCase):
    def test_portable_transform_matches_available_source(self):
        import json
        source = Path.home()/'.planar/projects/planar/scratch-codex-revision-fault-20261116'
        before = source/'run7/before'
        if not before.is_dir(): self.skipTest('project-local historical source unavailable')
        fixture = Path(__file__).resolve().parent/'fixture'
        inventory = json.loads((fixture/'inventory.json').read_text())['files']
        substitutions = {str(source/'codex-home/plugins/cache/planar-smoke-market/planar/0.1.0'):'/historical/planar-plugin',
                         str(source/'home/projects/revision-fault/tasks/T-001/worker-packet-1.md'):'/historical/worker-packet-1.md',
                         str(source/'repository'):'/workspace/repository'}
        for name, entry in inventory.items():
            with self.subTest(path=name):
                data=(before/name).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), entry['source_sha256'])
                replacements=[]
                for old,new in substitutions.items():
                    count=data.count(old.encode())
                    if count:
                        replacements.append({'replacement':new,'occurrences':count})
                        data=data.replace(old.encode(),new.encode())
                self.assertEqual(replacements,entry['transformations'])
                self.assertEqual(data,(fixture/name).read_bytes())

class VerifierBundleControls(unittest.TestCase):
    def test_bundle_matches_reviewed_draft_sources(self):
        case = Path(__file__).resolve().parent
        for name in ('assess.py', 'native.py', 'recovery_names.py'):
            self.assertEqual((case/name).read_bytes(), (case/'tests'/name).read_bytes())
        self.assertEqual((case/'recovery_names.py').read_bytes(), (case.parents[1]/'adapters/recovery_names.py').read_bytes())
        fixture = case/'fixture'; bundled = case/'tests/fixture'
        entries = [p.relative_to(fixture) for p in fixture.rglob('*') if p.is_file()]
        self.assertEqual(set(entries), {p.relative_to(bundled) for p in bundled.rglob('*') if p.is_file()})
        for name in entries: self.assertEqual((fixture/name).read_bytes(), (bundled/name).read_bytes())

if __name__ == '__main__': unittest.main()
