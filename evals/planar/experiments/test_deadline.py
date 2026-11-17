import json
import os
import sys
import tempfile
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'adapters'))
import launch
import watchdog


class DeadlineTest(unittest.TestCase):
    def test_expiry_during_preflight_never_starts_docker(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            image = 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7'
            snapshot = dict.fromkeys(('implementation_sha256', 'candidate_sha256', 'configuration_sha256', 'case_sha256'), 'test')
            snapshot.update(case='native-lineage-smoke', image_id=image)
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): ctx=launch.context()
            snapshot['prerequisites']={'selection_context':ctx.identity()}
            gate = {'selection_context':ctx.identity(), 'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1, **snapshot}
            review = root / 'review.json'
            review.write_text(json.dumps(gate))
            def slow(*args, **kwargs):
                time.sleep(.15)
                return snapshot
            end = (datetime.now(timezone.utc) + timedelta(seconds=.05)).isoformat()
            with patch.object(launch, 'context', return_value=ctx), patch.object(launch, 'plan', side_effect=slow), patch.object(launch, 'reviews_root', return_value=root), patch.object(launch.shutil, 'which', return_value='/fake/docker'), patch.object(launch.subprocess, 'run') as child:
                with self.assertRaises(TimeoutError):
                    launch.execute(root / 'auth', review, 'native-lineage-smoke', absolute_deadline=end)
                child.assert_not_called()

    def test_expired_watchdog_cleans_without_starting_child(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp)
            (evidence / 'recovery-required').write_text('pending')
            env = {'PLANAR_T044_EVIDENCE_DIR': temp, 'PLANAR_T044_PARENT_PID': str(os.getpid()),
                   'PLANAR_T044_ABSOLUTE_DEADLINE': (datetime.now(timezone.utc) - timedelta(seconds=1)).isoformat()}
            with patch.dict(os.environ, env), patch.object(watchdog, 'validate_owned_evidence',return_value=evidence.resolve()), patch.object(watchdog, 'recover_projects') as clean, patch.object(watchdog, 'supervise') as child:
                self.assertEqual(watchdog.main(), 2)
                child.assert_not_called()
                clean.assert_called_once()
                self.assertFalse((evidence / 'recovery-required').exists())

    def test_watchdog_uses_remaining_absolute_budget(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp)
            (evidence / 'recovery-required').write_text('pending')
            env = {'PLANAR_T044_EVIDENCE_DIR': temp, 'PLANAR_T044_PARENT_PID': str(os.getpid()),
                   'PLANAR_T044_DEADLINE_SECONDS': '1500',
                   'PLANAR_T044_ABSOLUTE_DEADLINE': (datetime.now(timezone.utc) + timedelta(seconds=2)).isoformat()}
            with patch.dict(os.environ, env), patch.object(watchdog, 'validate_owned_evidence',return_value=evidence.resolve()), patch.object(watchdog, 'supervise', return_value={'status': 'completed', 'return_code': 0}) as child:
                self.assertEqual(watchdog.main(), 2)
                self.assertGreater(child.call_args.kwargs['deadline'], 0)
                self.assertLessEqual(child.call_args.kwargs['deadline'], 2)
