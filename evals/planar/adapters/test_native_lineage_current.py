"""Offline current lineage freeze and historical contract controls."""
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import launch
from candidate import inventory


class CurrentLineageTest(unittest.TestCase):
    def test_current_freeze_and_historical_contract(self):
        old = launch.CASES['native-lineage-smoke']
        current = launch.CASES['native-lineage-current']
        manifest = json.loads((current / 'candidate.json').read_text())
        self.assertEqual(manifest, inventory(launch.CANDIDATE))
        self.assertEqual(manifest, json.loads((current / 'tests/bundle/manifest.json').read_text()))
        old_digest = json.loads((old / 'candidate.json').read_text())['sha256']
        new_digest = manifest['sha256']
        source = (current / 'tests/verify.py').read_text().replace(new_digest, old_digest).replace('planar/native-lineage-current', 'planar/native-lineage-smoke')
        self.assertEqual(source, (old / 'tests/verify.py').read_text())
        self.assertEqual((current / 'instruction.md').read_bytes(), (old / 'instruction.md').read_bytes())
        spec = importlib.util.spec_from_file_location('current_lineage_verifier', current / 'tests/verify.py')
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        verifier.manifest_check(current / 'tests/bundle')
        with self.assertRaisesRegex(verifier.EvidenceError, 'manifest mismatch'):
            verifier.manifest_check(old / 'tests/bundle')
        with tempfile.TemporaryDirectory() as temp:
            candidate = Path(temp) / 'candidate'
            shutil.copytree(launch.CANDIDATE, candidate)
            (candidate / 'LICENSE').write_text('stale')
            with self.assertRaisesRegex(ValueError, 'frozen inventory'):
                launch.plan('native-lineage-current', candidate_root=candidate, case_root=current)


if __name__ == '__main__':
    unittest.main()
