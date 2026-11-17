import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

CASE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('clean_revision_fixture', CASE / 'install_fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

class InstallTest(unittest.TestCase):
    def test_clean_projection(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = fixture.install(tmp)
            root = Path(tmp)
            self.assertEqual(result['projection_files'], 12)
            self.assertFalse((root / 'home/projects/revision-fault/tasks/T-001').exists())
            self.assertFalse((root / 'repository/STATE.txt').exists())
            self.assertFalse((root / 'repository/RESULT.txt').exists())
            expected = {p for p in fixture.PATHS if p.startswith('home/')}
            actual = {p.relative_to(root).as_posix() for p in (root / 'home').rglob('*') if p.is_file()}
            self.assertEqual(actual, expected)

    def test_packaged_projection_contains_no_historical_task_files(self):
        expected = set(fixture.PATHS) | {'inventory.json', 'base.bundle'}
        actual = {p.relative_to(fixture.SOURCE).as_posix() for p in fixture.SOURCE.rglob('*') if p.is_file()}
        self.assertEqual(actual, expected)
        self.assertFalse((fixture.SOURCE/'home/projects/revision-fault/tasks/T-001').exists())

    def test_changed_source_and_nonempty_workspace_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            shutil.copytree(fixture.SOURCE, source)
            (source / 'home/settings.yaml').write_text('changed\n')
            workspace = root / 'workspace'
            workspace.mkdir()
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                fixture.install(workspace, source)
            (workspace / 'other').touch()
            with self.assertRaisesRegex(ValueError, 'empty'):
                fixture.install(workspace)

    def test_projection_cannot_include_historical_task(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = json.loads((CASE / 'projection.json').read_text())
            data['files']['home/projects/revision-fault/tasks/T-001/record.md'] = '0' * 64
            manifest = root / 'bad.json'
            manifest.write_text(json.dumps(data))
            workspace = root / 'workspace'
            workspace.mkdir()
            with self.assertRaisesRegex(ValueError, 'identity mismatch'):
                fixture.install(workspace, manifest=manifest)

if __name__ == '__main__': unittest.main()
