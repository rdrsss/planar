"""Model-free batch orchestration with injected outcomes and fake gates."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import codex_run
import controls
import launch
from candidate import inventory
from harbor.models.task.task import Task


class BatchControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.experiment = self.root / 'experiment'
        case = launch.CASES['plugin-smoke']
        config = launch.config_for('batch', self.root, case)
        config['n_attempts'] = 2
        config['agents'] = [launch.config_for('batch', self.root, case, e)['agents'][0] for e in ('medium', 'high')]
        pins = {'candidate': inventory(launch.CANDIDATE)['sha256'], 'case': launch.case_inventory(case),
                'verifier': launch.case_inventory(case / 'tests'), 'image': Task(case).config.environment.docker_image,
                'host': 'codex-cli 0.169.0', 'case_name': 'plugin-smoke'}
        self.m = controls.initialize(self.experiment, config, mode='codex', hypothesis='injected batch control',
            baseline='a', variants=['a', 'b'], factors={'b': 'effort'}, pins=pins,
            variant_efforts={'a': 'medium', 'b': 'high'},
            caps={'session_seconds': 1200, 'verifier_seconds': 300, 'trial_seconds': 3600, 'job_seconds': 7200, 'trials': 5})
        self.gates = {}
        for row in self.m['trials']:
            path = self.root / (row['logical_id'].replace(':', '-') + '.json')
            path.write_text('{}')
            self.gates[row['logical_id']] = str(path)

    def test_stop_resume_and_explicit_retry_keep_primary(self):
        launched = []
        scope = {'stopped': False}
        def fake_execute(root, identity, logical, auth, gate):
            if scope['stopped']:
                raise ValueError('injected scoped stop')
            launched.append(logical)
            execution = controls.start(root, identity, logical)
            stop = logical == 'b:1'
            status = 'incomplete' if stop else 'pass'
            scope['stopped'] = stop
            controls.finish(root, execution['execution_id'], status, 'injected quota' if stop else 'injected pass')
            return {'execution_id': execution['execution_id'], 'status': status, 'stop_account': stop}
        with patch.object(codex_run, 'validate_gate'), patch.object(codex_run, 'execute_one', side_effect=fake_execute):
            first = codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', self.gates)
            self.assertEqual(launched, ['a:1', 'b:1'])
            self.assertEqual(first['not_dispatched'], ['a:2', 'b:2'])
            remaining = {k: self.gates[k] for k in first['not_dispatched']}
            with self.assertRaisesRegex(ValueError, 'scoped stop'):
                codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', remaining)
            self.assertEqual(launched, ['a:1', 'b:1'])
            scope['stopped'] = False  # injected explicit resolution
            resumed = codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', remaining)
            self.assertEqual(resumed['not_dispatched'], [])
            self.assertEqual(launched, ['a:1', 'b:1', 'a:2', 'b:2'])
            self.assertEqual(codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', {})['executions'], [])
        prior = next(e for e in controls.executions(self.experiment) if e['logical_id'] == 'b:1')
        retried = controls.start(self.experiment, self.m['identity'], 'b:1', retry_of=prior['execution_id'], reason='explicit injected retry')
        controls.finish(self.experiment, retried['execution_id'], 'pass', 'injected retry pass')
        result = controls.report(self.experiment, self.m['identity'])
        self.assertEqual(next(t for t in result['trials'] if t['logical_id'] == 'b:1')['reason'], 'injected quota')
        self.assertEqual(len(result['executions']), 5)

    def test_gate_binds_each_logical_identity_and_rejects_consumed(self):
        import hashlib
        fields = ('implementation_sha256', 'candidate_sha256', 'configuration_sha256', 'case', 'case_sha256', 'image_id', 'experiment_identity', 'experiment_implementation_sha256', 'admission_scope')
        common = dict.fromkeys(fields, 'test')
        gate = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1, **common, 'logical_id': 'a:1'}
        path = self.root / 'review.json'
        path.write_text(json.dumps(gate))
        def plan(root, identity, logical):
            return {**common, 'logical_id': logical}
        with patch.object(launch, 'reviews_root', return_value=self.root), patch.object(codex_run, 'preflight_one', side_effect=plan):
            codex_run.validate_gate(self.experiment, self.m['identity'], 'a:1', path)
            with self.assertRaisesRegex(ValueError, 'stale'):
                codex_run.validate_gate(self.experiment, self.m['identity'], 'b:1', path)
            digest = hashlib.sha256(json.dumps(gate, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            consumed = self.root / 'harbor-t044-consumed'
            consumed.mkdir()
            (consumed / digest).write_text('reserved')
            with self.assertRaisesRegex(ValueError, 'already consumed'):
                codex_run.validate_gate(self.experiment, self.m['identity'], 'a:1', path)

    def test_missing_duplicate_or_bad_gate_prevents_all_launches(self):
        with patch.object(codex_run, 'execute_one') as execute:
            with self.assertRaisesRegex(ValueError, 'each unreserved'):
                codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', {})
            duplicates = dict.fromkeys(self.gates, next(iter(self.gates.values())))
            with self.assertRaisesRegex(ValueError, 'distinct'):
                codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', duplicates)
            with patch.object(codex_run, 'validate_gate', side_effect=ValueError('bad gate')):
                with self.assertRaisesRegex(ValueError, 'bad gate'):
                    codex_run.execute_batch(self.experiment, self.m['identity'], self.root / 'not-read', self.gates)
            execute.assert_not_called()


if __name__ == '__main__':
    unittest.main()
