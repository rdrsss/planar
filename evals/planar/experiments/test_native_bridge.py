"""Exercise production START-hook bridge with a fake environment and nop."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class NativeBridgeTest(unittest.TestCase):
    def test_fresh_native_job_records_identity_and_refuses_resume(self):
        experiments = Path(__file__).resolve().parent
        adapters = experiments.parent / 'adapters'
        case = experiments.parent / 'cases/native-lineage-smoke'
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp).resolve() / ('planar-t044-' + 'a' * 32)
            evidence.mkdir()
            config = {'job_name': evidence.name, 'jobs_dir': str(evidence), 'n_attempts': 1,
                      'n_concurrent_trials': 1, 'retry': {'max_retries': 0},
                      'agents': [{'name': 'nop'}], 'tasks': [{'path': str(case)}],
                      'environment': {'import_path': 'fake_environment:FakeEnvironment'},
                      'verifier': {'disable': True}, 'quiet': True}
            path = evidence / 'job-config.json'
            path.write_text(json.dumps(config))
            env = {'PATH': os.environ['PATH'], 'HOME': temp, 'HARBOR_TELEMETRY': '0',
                   'PYTHONPATH': str(experiments)}
            command = [sys.executable, str(adapters / 'native_job.py'), str(path), str(evidence)]
            first = subprocess.run(command, env=env, capture_output=True, timeout=30)
            self.assertEqual(first.returncode, 0, first.stderr.decode())
            record = json.loads((evidence / 'native-trial-start.json').read_text())
            self.assertTrue(record['job_id'] and record['trial_id'])
            native_trial = Path(record['job_dir']) / record['trial_name'] / 'result.json'
            self.assertTrue(native_trial.is_file())
            # Inject verifier artifacts into an actual native nop result; this is
            # deterministic grading control, not evidence of a live verifier.
            from harbor.models.trial.result import TrialResult, TimingInfo
            from harbor.models.verifier.result import VerifierResult
            import native_status
            trial = TrialResult.model_validate_json(native_trial.read_text())
            trial.config.verifier.disable = False
            trial.verifier_environment_mode = 'separate'
            trial.verifier = TimingInfo(started_at=trial.started_at, finished_at=trial.finished_at)
            trial.verifier_result = VerifierResult(rewards={'reward': 1})
            verifier = Path(temp) / 'injected-verifier'
            verifier.mkdir()
            (verifier / 'reward.txt').write_text('1')
            (verifier / 'status.json').write_text(json.dumps({'status': 'pass'}))
            self.assertEqual(native_status.assess_verified(trial, verifier)['status'], 'pass')
            (verifier / 'status.json').write_text(json.dumps({'status': 'behavioral_failure'}))
            self.assertEqual(native_status.assess_verified(trial, verifier)['status'], 'incomplete')
            trial.verifier_result = VerifierResult(rewards={'reward': 0})
            (verifier / 'reward.txt').write_text('0')
            self.assertEqual(native_status.assess_verified(trial, verifier)['status'], 'behavioral_failure')
            # Copied native identities are rejected before a summary is written.
            import codex_run
            import controls
            import launch
            from unittest.mock import patch
            root = Path(temp).resolve()
            folder = root / 'experiment' / 'executions' / ('a' * 32)
            folder.mkdir(parents=True)
            controls.create(folder / 'start.json', {'execution_id': 'a' * 32})
            controls.create(folder / 'dispatch.json', {'launcher_id': evidence.name, 'evidence': str(evidence)})
            for name in ('launcher-terminal.json', 'watchdog-terminal.json'):
                (evidence / name).write_text(json.dumps({'status': 'completed', 'return_code': 0}))
            records = root / 'planar-account-admission-v1/records'
            records.mkdir(parents=True)
            # Cleanup/identity checks must run before any verifier grading.
            marker = evidence / 'recovery-required'
            marker.write_text('pending')
            with patch.object(launch, 'reviews_root', return_value=root), patch.object(launch, 'jobs_root', return_value=root), patch.object(codex_run.native_status, 'assess_verified') as grade:
                self.assertIsNone(codex_run.reconcile(folder))
                grade.assert_not_called()
            marker.unlink()
            controls.create(records / ('b' * 32 + '.start.json'), {'execution_id': 'b' * 32})
            other = root / ('planar-t044-' + 'b' * 32)
            other.mkdir()
            (other / 'native-trial-start.json').write_bytes((evidence / 'native-trial-start.json').read_bytes())
            with patch.object(launch, 'reviews_root', return_value=root), patch.object(launch, 'jobs_root', return_value=root):
                self.assertIsNone(codex_run.reconcile(folder))
                self.assertFalse((folder / 'terminal.json').exists())
                (records / ('b' * 32 + '.start.json')).unlink()
                native_path = evidence / 'native-trial-start.json'
                original = native_path.read_text()
                altered = json.loads(original)
                altered['trial_name'] = 'contradictory'
                native_path.write_text(json.dumps(altered))
                self.assertIsNone(codex_run.reconcile(folder))
                self.assertFalse((folder / 'terminal.json').exists())
                native_path.write_text(original)
                controls.create(records / ('a' * 32 + '.start.json'), {'execution_id': 'a' * 32})
                job_path = Path(record['job_dir']) / 'result.json'
                trial_path = Path(record['job_dir']) / record['trial_name'] / 'result.json'
                job_original = job_path.read_text()
                trial_original = trial_path.read_text()
                altered_job = json.loads(job_original)
                altered_job['stats'].update(n_completed_trials=0, n_running_trials=0, n_pending_trials=1)
                job_path.write_text(json.dumps(altered_job))
                self.assertIsNone(codex_run.reconcile(folder))
                self.assertFalse((folder / 'terminal.json').exists())
                self.assertEqual(len(codex_run.shared_pending(records)), 1)
                self.assertEqual(json.loads(job_path.read_text())['stats']['n_pending_trials'], 1)
                job_path.write_text(job_original)
                cross_record = json.loads(job_original)
                cross_record['finished_at'] = cross_record['started_at']
                job_path.write_text(json.dumps(cross_record))
                before_cross = job_path.read_bytes()
                self.assertIsNone(codex_run.reconcile(folder))
                self.assertFalse((folder / 'terminal.json').exists())
                self.assertEqual(len(codex_run.shared_pending(records)), 1)
                self.assertEqual(job_path.read_bytes(), before_cross)
                job_path.write_text(job_original)
                for key in ('n_errored_trials', 'n_cancelled_trials', 'n_retries'):
                    inconsistent = json.loads(job_original)
                    inconsistent['stats'][key] = 1
                    job_path.write_text(json.dumps(inconsistent))
                    self.assertIsNone(codex_run.reconcile(folder), key)
                    self.assertFalse((folder / 'terminal.json').exists())
                job_path.write_text(job_original)
                altered_trial = json.loads(trial_original)
                altered_trial['finished_at'] = '1970-01-01T00:00:00Z'
                trial_path.write_text(json.dumps(altered_trial))
                self.assertIsNone(codex_run.reconcile(folder))
                self.assertFalse((folder / 'terminal.json').exists())
                self.assertEqual(len(codex_run.shared_pending(records)), 1)
                self.assertEqual(json.loads(trial_path.read_text())['finished_at'], '1970-01-01T00:00:00Z')
                trial_path.write_text(trial_original)
                self.assertEqual(codex_run.reconcile(folder)['status'], 'incomplete')
            before = (evidence / 'native-trial-start.json').read_bytes()
            second = subprocess.run(command, env=env, capture_output=True, timeout=30)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(before, (evidence / 'native-trial-start.json').read_bytes())


if __name__ == '__main__':
    unittest.main()
