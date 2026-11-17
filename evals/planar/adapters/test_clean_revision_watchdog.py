"""Confirm the selected whole-job bound reaches existing supervision."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import watchdog

class WatchdogTest(unittest.TestCase):
    def test_1050_seconds_and_cleanup_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'recovery-required').write_text('pending')
            env={'PLANAR_T044_EVIDENCE_DIR':str(root), 'PLANAR_T044_PARENT_PID':str(os.getpid()), 'PLANAR_T044_DEADLINE_SECONDS':'1050'}
            calls=[]
            def supervise(command, **kwargs):
                calls.append(kwargs); kwargs['cleanup']()
                return {'status':'interrupted','reason':'TimeoutExpired'}
            with patch.dict(os.environ,env,clear=True), patch.object(watchdog,'validate_owned_evidence',return_value=root.resolve()), patch.object(watchdog,'supervise',side_effect=supervise), patch.object(watchdog,'recover_projects') as recovery:
                self.assertEqual(watchdog.main(),2)
            self.assertEqual(calls[0]['deadline'],1050)
            self.assertEqual(calls[0]['grace'],15)
            recovery.assert_called_once_with(root.resolve())
            self.assertFalse((root/'recovery-required').exists())
            self.assertEqual(json.loads((root/'watchdog-terminal.json').read_text())['reason'],'TimeoutExpired')

    def test_watchdog_records_trusted_start_and_effective_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); (root/'recovery-required').write_text('pending')
            env={'PLANAR_T044_EVIDENCE_DIR':str(root), 'PLANAR_T044_PARENT_PID':str(os.getpid()),
                 'PLANAR_T044_DEADLINE_SECONDS':'1050', 'PLANAR_T044_CASE':'clean-revision',
                 'PLANAR_T044_ABSOLUTE_DEADLINE':'2100-01-01T00:00:00+00:00'}
            def supervise(command, **kwargs):
                kwargs['on_start'](1234); kwargs['cleanup']()
                return {'status':'completed','return_code':0}
            with patch.dict(os.environ,env,clear=True), patch.object(watchdog,'validate_owned_evidence',return_value=root.resolve()), patch.object(watchdog,'supervise',side_effect=supervise), patch.object(watchdog,'recover_projects'):
                watchdog.main()
            record=json.loads((root/'harbor-process.json').read_text())
            self.assertEqual(record['execution_id'],root.name)
            self.assertEqual(record['deadline_seconds'],1050)
            self.assertGreater(record['started_monotonic_ns'],0)
            self.assertEqual(record['absolute_deadline'],env['PLANAR_T044_ABSOLUTE_DEADLINE'])

if __name__=='__main__': unittest.main()
