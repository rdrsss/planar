"""Durable account admission controls, with injected typed-status records."""
import tempfile
import os
import unittest
from pathlib import Path
import codex_run
import controls


class AccountStopTest(unittest.TestCase):
    def test_crash_after_classification_still_blocks(self):
        for reason in ('native usage limit', 'native authentication error'):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                folder = root / 'executions' / 'abc'
                folder.mkdir(parents=True)
                controls.create(folder / 'native-result.json', {'classification': {'stop_account': True, 'reason': reason}})
                self.assertEqual(codex_run.pending_account_stops(root), ['abc'])
                controls.create(folder / 'account-stop-resolved.json', {'reason': 'explicit fixture resolution'})
                self.assertEqual(codex_run.pending_account_stops(root), [])

    def test_shared_scope_rejects_two_controllers(self):
        from unittest.mock import patch
        import launch
        with tempfile.TemporaryDirectory() as temp, patch.object(launch, 'reviews_root', return_value=Path(temp)):
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':temp}): selected=launch.context()
            context_patch=patch.object(launch,'context',return_value=selected); context_patch.start(); self.addCleanup(context_patch.stop)
            with codex_run.admission_scope() as records:
                self.assertEqual(records,Path(temp)/'planar-account-admission-v1'/'records')
                with self.assertRaises(BlockingIOError):
                    with codex_run.admission_scope():
                        pass
                controls.create(records / 'first.start.json', {'execution_id': 'first', 'experiment': 'A'})
                self.assertEqual(len(codex_run.shared_pending(records)), 1)
            with codex_run.admission_scope() as records:
                self.assertEqual(len(codex_run.shared_pending(records)), 1)
                controls.create(records / 'first.resolved.json', {'reason': 'explicit fixture resolution'})
                self.assertEqual(codex_run.shared_pending(records), [])
