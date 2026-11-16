import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'cases/plugin-smoke/tests/verify.py'
spec = importlib.util.spec_from_file_location('smoke_verify', path)
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class ResultControls(unittest.TestCase):
    def test_absent_wrong_extra_and_symlink_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = root / 'RESULT.txt'
            self.assertIsNotNone(verify.result_check(root))
            result.write_bytes(b'2')
            self.assertIsNotNone(verify.result_check(root))
            result.write_bytes(b'2\n')
            self.assertIsNone(verify.result_check(root))
            (root / 'unexpected').write_bytes(b'')
            self.assertIsNotNone(verify.result_check(root))
            (root / 'unexpected').unlink()
            result.unlink()
            result.symlink_to('/etc/passwd')
            self.assertIsNotNone(verify.result_check(root))

    def test_exception_and_missing_sessions_fail_closed(self):
        with patch.object(verify, 'assess', side_effect=OSError('failure')):
            self.assertEqual(verify.main(), 2)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'RESULT.txt').write_bytes(b'2\n')
            with self.assertRaises(ValueError):
                verify.assess(root, root / 'missing')

    def test_frozen_references_and_packaged_grader_match_sources(self):
        root = path.parents[3]
        self.assertEqual(set(verify.expected_files(verify.BUNDLE)), set(verify.FILES))
        for name in ('regrade_dispatch.py', 'candidate.py', 'check_provenance.py'):
            self.assertEqual((verify.BUNDLE / name).read_bytes(), (root / 'adapters' / name).read_bytes())
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / 'bundle'
            shutil.copytree(verify.BUNDLE, bundle)
            (bundle / 'candidate/agents/worker.md').write_text('tampered')
            with self.assertRaises(ValueError):
                verify.expected_files(bundle)
            shutil.copytree(verify.BUNDLE, bundle, dirs_exist_ok=True)
            manifest = json.loads((bundle / 'manifest.json').read_text())
            manifest['files'][0]['sha256'] = '0' * 64
            (bundle / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                verify.expected_files(bundle)
