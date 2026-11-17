"""Offline snapshot gate checks; no auth or host processes."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'adapters'))
import launch
from candidate import inventory

def actual_candidate_digest():
    return inventory(launch.CANDIDATE)['sha256']


class SnapshotGateTest(unittest.TestCase):
    def test_snapshot_config_and_candidate_must_match_gate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            case = root / 'case'
            candidate = root / 'candidate'
            shutil.copytree(launch.CASES['plugin-smoke'], case)
            (candidate / '.claude-plugin').mkdir(parents=True)
            (candidate / '.claude-plugin/plugin.json').write_text('{}')
            expected = json.loads((case / 'candidate.json').read_text())
            with patch.object(launch, 'inventory', return_value=expected), patch.object(launch, 'reviews_root', return_value=root):
                with self.assertRaisesRegex(ValueError, 'supplied together'):
                    launch.plan(candidate_root=candidate)
                reviewed = launch.plan(candidate_root=candidate, case_root=case)
                self.assertEqual(reviewed['configuration']['tasks'][0]['path'], str(case.resolve()))
                gate = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1,
                        **{k: reviewed[k] for k in ('implementation_sha256', 'candidate_sha256', 'configuration_sha256', 'case', 'case_sha256', 'image_id')}}
                gate_path = root / 'gate.json'
                gate_path.write_text(json.dumps(gate))
                with self.assertRaisesRegex(ValueError, 'stale'):
                    launch.execute(root / 'no-auth', gate_path, candidate_root=candidate, case_root=case, trial_deadline=60)
                with self.assertRaisesRegex(ValueError, 'stale'):
                    launch.execute(root / 'no-auth', gate_path, candidate_root=candidate, case_root=case, effort='high')
                with self.assertRaisesRegex(ValueError, 'unsupported effort'):
                    launch.plan('fresh-interrupted-review', effort='high')
                with self.assertRaisesRegex(ValueError, 'invalid reserved'):
                    launch.execute(root / 'no-auth', gate_path, candidate_root=candidate, case_root=case, execution_id='../bad')
                (case / 'instruction.md').write_text('changed')
                with self.assertRaisesRegex(ValueError, 'stale'):
                    launch.execute(root / 'no-auth', gate_path, candidate_root=candidate, case_root=case)


if __name__ == '__main__':
    unittest.main()

class CodexDefinitionTest(unittest.TestCase):
    def test_native_variant_definition(self):
        import controls
        with tempfile.TemporaryDirectory() as temp:
            base = launch.config_for('comparison', Path(temp), launch.CASES['plugin-smoke'])
            base['n_attempts'] = 2
            base['agents'] = [launch.config_for('comparison', Path(temp), launch.CASES['plugin-smoke'], effort)['agents'][0] for effort in ('medium', 'high')]
            from harbor.models.task.task import Task
            case = launch.CASES['plugin-smoke']
            pins = {'candidate': actual_candidate_digest(), 'case': launch.case_inventory(case), 'verifier': launch.case_inventory(case / 'tests'), 'image': Task(case).config.environment.docker_image, 'host': 'codex-cli 0.169.0', 'case_name': 'plugin-smoke'}
            m = controls.initialize(Path(temp) / 'experiment', base, mode='codex',
                hypothesis='test definition only', baseline='medium', variants=['medium', 'high'],
                factors={'high': 'effort'}, pins=pins, variant_efforts={'medium': 'medium', 'high': 'high'},
                caps={'session_seconds': 60, 'verifier_seconds': 60, 'trial_seconds': 60, 'job_seconds': 300, 'trials': 4})
            self.assertEqual(m['resolved_pins']['case'], pins['case'])
            bad_pins = {**pins, 'candidate': 'wrong'}
            with self.assertRaisesRegex(ValueError, 'contradict'):
                controls.initialize(Path(temp) / 'bad', base, mode='codex', hypothesis='test', baseline='medium', variants=['medium', 'high'], factors={'high': 'effort'}, pins=bad_pins, variant_efforts={'medium': 'medium', 'high': 'high'}, caps={'session_seconds': 60, 'verifier_seconds': 60, 'trial_seconds': 60, 'job_seconds': 300, 'trials': 4})
            self.assertEqual(m['kind'], 'codex-experiment')
            self.assertEqual([t['logical_id'] for t in m['trials']], ['medium:1', 'high:1', 'medium:2', 'high:2'])
            with self.assertRaisesRegex(ValueError, 'nop runner'):
                controls.run_nop(Path(temp) / 'experiment', m['identity'])

class CodexDispatchTest(unittest.TestCase):
    def test_missing_native_evidence_never_becomes_pass(self):
        import controls
        import codex_run
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): selected=launch.context()
            context_patch=patch.object(launch,'context',return_value=selected); context_patch.start(); self.addCleanup(context_patch.stop)
            base = launch.config_for('comparison', root, launch.CASES['plugin-smoke'])
            base['agents'] = [launch.config_for('comparison', root, launch.CASES['plugin-smoke'], e)['agents'][0] for e in ('medium', 'high')]
            from harbor.models.task.task import Task
            case = launch.CASES['plugin-smoke']
            pins = {'candidate': actual_candidate_digest(), 'case': launch.case_inventory(case), 'verifier': launch.case_inventory(case / 'tests'), 'image': Task(case).config.environment.docker_image, 'host': 'codex-cli 0.169.0', 'case_name': 'plugin-smoke'}
            experiment = root / 'experiment'
            m = controls.initialize(experiment, base, mode='codex', hypothesis='test', baseline='a', variants=['a', 'b'],
                factors={'b': 'effort'}, pins=pins, variant_efforts={'a': 'medium', 'b': 'high'},
                caps={'session_seconds': 60, 'verifier_seconds': 60, 'trial_seconds': 60, 'job_seconds': 300, 'trials': 3})
            fields = ('implementation_sha256', 'candidate_sha256', 'configuration_sha256', 'case', 'case_sha256', 'image_id', 'experiment_identity', 'logical_id', 'experiment_implementation_sha256', 'admission_scope')
            preflight = dict.fromkeys(fields, 'test')
            gate = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1, **preflight}
            (root / 'review').write_text(json.dumps(gate))
            import cli
            import io
            from contextlib import redirect_stdout
            output = io.StringIO()
            argv = ['cli', 'codex-run', str(experiment), m['identity'], 'a:1', '--auth-file', str(root / 'auth'), '--review-file', str(root / 'review')]
            with patch.object(codex_run, 'preflight_one', return_value=preflight), patch.object(launch, 'reviews_root', return_value=root), patch.object(launch, 'jobs_root', return_value=root / 'jobs'), patch.object(launch, 'execute', return_value=2) as execute, patch.object(sys, 'argv', argv), redirect_stdout(output):
                cli.main()
            result = json.loads(output.getvalue())
            self.assertFalse(execute.call_args.kwargs['emit'])
            self.assertEqual(result['status'], 'incomplete')
            self.assertTrue(result['stop_account'])
            self.assertTrue(execute.called)
            self.assertEqual(controls.report(experiment, m['identity'])['groups'][0]['pass'], 0)
            self.assertEqual(codex_run.pending_account_stops(experiment), [result['execution_id']])
            second = root / 'second-experiment'
            m2 = controls.initialize(second, base, mode='codex', hypothesis='second test', baseline='a', variants=['a', 'b'],
                factors={'b': 'effort'}, pins=pins, variant_efforts={'a': 'medium', 'b': 'high'},
                caps={'session_seconds': 60, 'verifier_seconds': 60, 'trial_seconds': 60, 'job_seconds': 300, 'trials': 3})
            with patch.object(launch, 'reviews_root', return_value=root), patch.object(launch, 'execute') as blocked:
                with self.assertRaisesRegex(ValueError, 'account stop'):
                    codex_run.execute_one(second, m2['identity'], 'a:1', root / 'auth', root / 'review')
                blocked.assert_not_called()
            with patch.object(launch, 'reviews_root', return_value=root):
                with self.assertRaisesRegex(ValueError, 'account stop'):
                    codex_run.execute_one(experiment, m['identity'], 'b:1', root / 'auth', root / 'review')
            with patch.object(launch, 'reviews_root', return_value=root):
                codex_run.resolve_account_stop(experiment, m['identity'], result['execution_id'], 'investigated and cleared')
            self.assertEqual(codex_run.pending_account_stops(experiment), [])
