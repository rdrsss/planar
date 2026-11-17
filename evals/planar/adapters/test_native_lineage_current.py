"""Offline current lineage freeze and historical contract controls."""
import importlib.util
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import launch
from candidate import inventory


class CurrentLineageTest(unittest.TestCase):
    def test_frozen_case_is_consistent_and_preserves_historical_contract(self):
        old = launch.CASES['native-lineage-smoke']
        current = launch.CASES['native-lineage-current']
        manifest = json.loads((current / 'candidate.json').read_text())
        canonical = json.dumps(manifest['files'], ensure_ascii=True,
                               sort_keys=True, separators=(',', ':')).encode()
        self.assertEqual(manifest['algorithm'], 'sha256-json-inventory-v1')
        self.assertEqual(manifest['sha256'], hashlib.sha256(canonical).hexdigest())
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

    def test_actual_candidate_drift_is_refused_before_configuration(self):
        # The fixture isolates inventory admission. It is not a new case freeze
        # or a claim that the historical verifier accepts this synthetic plugin.
        with tempfile.TemporaryDirectory() as temp:
            candidate = Path(temp) / 'candidate'
            (candidate / '.claude-plugin').mkdir(parents=True)
            (candidate / '.claude-plugin/plugin.json').write_text('{}')
            (candidate / 'LICENSE').write_text('original')
            case = Path(temp) / 'case'
            case.mkdir()
            (case / 'candidate.json').write_text(json.dumps(inventory(candidate)))
            with patch.object(launch, 'config_for',
                              side_effect=RuntimeError('inventory admitted')) as configure:
                with self.assertRaisesRegex(RuntimeError, 'inventory admitted'):
                    launch.plan('native-lineage-current', candidate_root=candidate,
                                case_root=case)
                configure.assert_called_once()
            (candidate / 'LICENSE').write_text('stale')
            with patch.object(launch, 'config_for') as configure:
                with self.assertRaisesRegex(ValueError, 'frozen inventory'):
                    launch.plan('native-lineage-current', candidate_root=candidate,
                                case_root=case)
                configure.assert_not_called()


if __name__ == '__main__':
    unittest.main()
