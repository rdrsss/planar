import tempfile
import unittest
from pathlib import Path

import controls

CASE = Path(__file__).resolve().parents[1] / 'cases/native-lineage-smoke'


class ControlsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'experiment'
        config = {'job_name': 'control', 'jobs_dir': self.temp.name, 'n_attempts': 2,
                  'n_concurrent_trials': 1, 'retry': {'max_retries': 0},
                  'agents': [{'name': 'nop'}, {'name': 'nop'}], 'tasks': [{'path': str(CASE)}]}
        self.m = controls.initialize(self.root, config, hypothesis='control', baseline='a',
            variants=['a', 'b'], factors={'b': 'changed'},
            pins=dict.fromkeys(('candidate', 'case', 'verifier', 'image', 'host'), 'test-pin'),
            caps={'session_seconds': 1200, 'verifier_seconds': 300, 'trial_seconds': 3600,
                  'job_seconds': 7200, 'trials': 4})
        self.identity = self.m['identity']

    def test_interruption_retry_and_primary_counts(self):
        self.assertEqual([x['logical_id'] for x in self.m['trials']], ['a:1', 'b:1', 'a:2', 'b:2'])
        first = controls.start(self.root, self.identity, 'a:1')
        controls.finish(self.root, first['execution_id'], 'pass', 'injected pass')
        interrupted = controls.start(self.root, self.identity, 'b:1')
        with self.assertRaises(ValueError):
            controls.start(self.root, self.identity, 'b:1')
        retried = controls.start(self.root, self.identity, 'b:1', retry_of=interrupted['execution_id'], reason='selected')
        controls.finish(self.root, retried['execution_id'], 'pass', 'injected pass')
        a2 = controls.start(self.root, self.identity, 'a:2')
        controls.finish(self.root, a2['execution_id'], 'error', 'injected authentication error')
        b2 = controls.start(self.root, self.identity, 'b:2')
        controls.finish(self.root, b2['execution_id'], 'incomplete', 'injected usage limit')
        result = controls.report(self.root, self.identity)
        self.assertEqual(len(result['executions']), 5)
        self.assertEqual(result['groups'][1]['incomplete'], 2)
        self.assertIsNone(result['groups'][1]['pass_rate'])
        self.assertIn('undefined', controls.markdown(result))
        with self.assertRaises(FileExistsError):
            controls.finish(self.root, first['execution_id'], 'pass', 'duplicate')
        with self.assertRaises(ValueError):
            controls.start(self.root, 'changed', 'a:1')

    def test_primary_reason_does_not_follow_retry_uuid_order(self):
        from unittest.mock import patch
        from uuid import UUID
        with patch.object(controls, 'uuid4', side_effect=[UUID(hex='f' * 32), UUID(hex='a' * 32)]):
            primary = controls.start(self.root, self.identity, 'a:1')
            controls.finish(self.root, primary['execution_id'], 'incomplete', 'primary quota')
            retry = controls.start(self.root, self.identity, 'a:1', retry_of=primary['execution_id'], reason='selected')
            controls.finish(self.root, retry['execution_id'], 'pass', 'retry pass')
        result = controls.report(self.root, self.identity)
        self.assertEqual(result['groups'][0]['incomplete'], 1)
        self.assertEqual(next(t for t in result['trials'] if t['logical_id'] == 'a:1')['reason'], 'primary quota')
        self.assertIn('- a:1: primary quota', controls.markdown(result))
        self.assertTrue(any(e['reason'] == 'retry pass' for e in result['executions']))

    def test_pending_and_missing_result(self):
        controls.start(self.root, self.identity, 'a:1')
        result = controls.report(self.root, self.identity)
        self.assertEqual(result['groups'][0]['incomplete'], 1)
        self.assertEqual(result['groups'][0]['not_started'], 1)
        self.assertEqual(result['groups'][0]['reserved'], 1)
        self.assertEqual(result['groups'][0]['started'], 0)
        self.assertEqual(result['caps']['trials'], 4)
        self.assertEqual(len(result['configured_agents']), 2)
        text = controls.markdown(result)
        for trial_id in ('a:1', 'b:1', 'a:2', 'b:2'):
            self.assertIn(trial_id, text)
        self.assertIn('not started: no execution reserved', text)
        self.assertIn('trial_seconds', text)
        self.assertIsNone(result['groups'][0]['pass_rate'])


if __name__ == '__main__':
    unittest.main()

class NativeQueueTest(unittest.TestCase):
    def test_native_serial_queue_with_fake_trials(self):
        """Exercise Harbor's actual queue and zero-retry semantics without a model."""
        import asyncio
        from unittest.mock import patch
        from types import SimpleNamespace
        from harbor.models.job.config import RetryConfig
        from harbor.trial.queue import TrialQueue
        from harbor.trial.trial import Trial

        active = 0
        peak = 0
        starts = []
        configs = [SimpleNamespace(trial_name=n, agent=SimpleNamespace(n_concurrent=None))
                   for n in ('a:1', 'b:1', 'a:2', 'b:2')]

        class FakeTrial:
            def __init__(self, config):
                self.config = config
            def add_hook(self, event, callback):
                pass
            async def run(self):
                nonlocal active, peak
                active += 1
                peak = max(peak, active)
                starts.append(self.config.trial_name)
                await asyncio.sleep(0)
                active -= 1
                return SimpleNamespace(recorded_exceptions=lambda: [])

        async def create(config):
            return FakeTrial(config)

        async def run():
            queue = TrialQueue(n_concurrent=1, retry_config=RetryConfig(max_retries=0))
            with patch.object(Trial, 'create', side_effect=create):
                return await asyncio.gather(*queue.submit_batch(configs))

        self.assertEqual(len(asyncio.run(run())), 4)
        self.assertEqual(starts, ['a:1', 'b:1', 'a:2', 'b:2'])
        self.assertEqual(peak, 1)

class NativeExecutionTest(ControlsTest):
    def test_reconcile_native_result_after_summary_crash(self):
        self.assertEqual(controls.run_nop(self.root, self.identity, max_new=1), 1)
        folder = next((self.root / 'executions').iterdir())
        (folder / 'terminal.json').unlink()  # Simulate interruption before terminal record.
        import json
        native = controls.read(folder / 'native-start.json')
        job_path = Path(native['raw_job']) / 'result.json'
        trial_path = Path(native['raw_job']) / native['trial_name'] / 'result.json'
        job_original = job_path.read_text()
        trial_original = trial_path.read_text()
        altered = json.loads(job_original)
        altered['stats'].update(n_completed_trials=0, n_running_trials=0, n_pending_trials=1)
        job_path.write_text(json.dumps(altered))
        controls.reconcile_nop(self.root)
        self.assertFalse((folder / 'terminal.json').exists())
        job_path.write_text(job_original)
        altered = json.loads(job_original)
        altered['finished_at'] = altered['started_at']
        job_path.write_text(json.dumps(altered))
        before = job_path.read_bytes()
        controls.reconcile_nop(self.root)
        self.assertFalse((folder / 'terminal.json').exists())
        self.assertEqual(job_path.read_bytes(), before)
        job_path.write_text(job_original)
        altered = json.loads(trial_original)
        altered['finished_at'] = '1970-01-01T00:00:00Z'
        trial_path.write_text(json.dumps(altered))
        controls.reconcile_nop(self.root)
        self.assertFalse((folder / 'terminal.json').exists())
        trial_path.write_text(trial_original)
        controls.reconcile_nop(self.root)
        self.assertEqual(controls.read(folder / 'terminal.json')['reason'], 'reconciled nop native results: verifier disabled')
        controls.reconcile_nop(self.root)
        self.assertEqual(len(controls.executions(self.root)), 1)

    def test_ambiguous_nop_stops_dispatch_until_explicit_resolution(self):
        import hashlib
        import json
        from unittest.mock import patch
        import supervise
        self.assertEqual(controls.run_nop(self.root, self.identity, max_new=1), 1)
        folder = next((self.root / 'executions').iterdir())
        (folder / 'terminal.json').unlink()  # Simulate a crash before summary.
        native = controls.read(folder / 'native-start.json')
        raw = Path(native['raw_job'])
        job_path = raw / 'result.json'
        altered = json.loads(job_path.read_text())
        altered['stats'].update(n_completed_trials=0, n_running_trials=0, n_pending_trials=1)
        job_path.write_text(json.dumps(altered))
        def raw_hashes():
            return {str(p.relative_to(raw)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in raw.rglob('*') if p.is_file()}
        before = raw_hashes()
        with patch.object(supervise, 'supervise') as host:
            with self.assertRaisesRegex(ValueError, 'ambiguous nop execution'):
                controls.run_nop(self.root, self.identity, max_new=1)
            host.assert_not_called()
        self.assertEqual(len(list((self.root / 'executions').iterdir())), 1)
        self.assertEqual(raw_hashes(), before)
        self.assertFalse((folder / 'terminal.json').exists())
        eid = controls.read(folder / 'start.json')['execution_id']
        controls.resolve_nop_stop(self.root, self.identity, eid, 'reviewed ambiguous fixture')
        self.assertEqual(controls.read(folder / 'account-stop-resolved.json')['reason'], 'reviewed ambiguous fixture')
        self.assertEqual(controls.run_nop(self.root, self.identity, max_new=1), 1)
        self.assertEqual(raw_hashes(), before)
        self.assertFalse((folder / 'terminal.json').exists())

    def test_injected_limit_stops_later_native_jobs(self):
        self.assertEqual(controls.run_nop(self.root, self.identity, injected_events={'b:1': 'usage_limit'}), 2)
        rows = controls.executions(self.root)
        self.assertEqual(len(rows), 2)
        with self.assertRaisesRegex(ValueError, 'explicit resolution'):
            controls.run_nop(self.root, self.identity)
        stopped = next(e for e in rows if e['reason'] == 'injected usage limit')
        controls.resolve_nop_stop(self.root, self.identity, stopped['execution_id'], 'injected limit cleared')
        self.assertEqual(controls.run_nop(self.root, self.identity), 2)
        self.assertEqual(sum(e['reason'] == 'injected usage limit' for e in rows), 1)
        self.assertEqual(sum(g['not_started'] for g in controls.report(self.root, self.identity)['groups']), 0)

    def test_nop_explicit_retry_and_snapshot_tamper(self):
        self.assertEqual(controls.run_nop(self.root, self.identity, max_new=1), 1)
        first = controls.executions(self.root)[0]
        self.assertEqual(controls.run_nop(self.root, self.identity, retry_of=first['execution_id'], retry_reason='selected control'), 1)
        rows = controls.executions(self.root)
        self.assertEqual(len(rows), 2)
        self.assertEqual(sum(row['predecessor'] == first['execution_id'] for row in rows), 1)
        self.assertEqual(controls.report(self.root, self.identity)['groups'][0]['started'], 1)
        frozen = next((self.root / 'case-snapshot').rglob('*.md'))
        frozen.write_text(frozen.read_text() + '\ntamper\n')
        with self.assertRaises(ValueError):
            controls.run_nop(self.root, self.identity)

    def test_real_nop_jobs_interruption_and_continuation(self):
        import hashlib
        self.assertEqual(controls.run_nop(self.root, self.identity, max_new=1), 1)
        first = next((self.root / 'executions').iterdir())
        native = controls.read(first / 'native-start.json')
        self.assertTrue(native['trial_id'])
        self.assertTrue(native['job_id'])
        raw = Path(native['raw_job']) / 'result.json'
        before = hashlib.sha256(raw.read_bytes()).hexdigest()
        self.assertEqual(controls.run_nop(self.root, self.identity), 3)
        self.assertEqual(controls.run_nop(self.root, self.identity), 0)
        self.assertEqual(hashlib.sha256(raw.read_bytes()).hexdigest(), before)
        result = controls.report(self.root, self.identity)
        self.assertEqual(len(result['executions']), 4)
        self.assertTrue(all(g['incomplete'] == 2 for g in result['groups']))
