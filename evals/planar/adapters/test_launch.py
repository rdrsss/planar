import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import launch
from candidate import inventory as actual_inventory


class LaunchControls(unittest.TestCase):
    def setUp(self):
        def fixture(_):
            return json.loads((launch.CASES[self.case_name] / 'candidate.json').read_text())
        self.case_name = 'plugin-smoke'
        self.inventory_patch = patch.object(launch, 'inventory', side_effect=fixture)
        self.inventory_patch.start()
        self.addCleanup(self.inventory_patch.stop)

    def test_output_root_override_reaches_plan_and_recovery(self):
        import os
        from recover import jobs_root
        with tempfile.TemporaryDirectory(prefix='planar output ') as temp:
            with patch.dict(os.environ, {'PLANAR_EVALS_OUTPUT_ROOT': temp}), patch.object(launch, 'PROJECT', Path(temp) / 'absent-sibling'), patch.object(launch, 'REVIEWS', Path(temp) / 'absent-sibling' / 'reviews'):
                snapshot = launch.plan()
                self.assertEqual(snapshot['prerequisites']['evidence_write_root'], str(Path(temp).resolve() / 'jobs'))
                self.assertEqual(jobs_root(), Path(temp).resolve() / 'jobs')
                self.assertEqual(snapshot['configuration']['jobs_dir'], str(Path(temp).resolve() / 'jobs'))
                self.assertEqual(snapshot['prerequisites']['write_destinations']['harbor-t044-consumed']['path'], str(Path(temp).resolve() / 'harbor-t044-consumed'))

    def test_output_aliases_are_rejected_and_roots_bind_gate_digest(self):
        import os
        with tempfile.TemporaryDirectory() as temp:
            a, b = Path(temp) / 'a', Path(temp) / 'b'
            a.mkdir(); b.mkdir(); (a / 'jobs').mkdir()
            with patch.dict(os.environ, {'PLANAR_EVALS_OUTPUT_ROOT': str(a)}):
                first = launch.plan()['configuration_sha256']
            with patch.dict(os.environ, {'PLANAR_EVALS_OUTPUT_ROOT': str(b)}):
                snapshot = launch.plan()
                self.assertNotEqual(first, snapshot['configuration_sha256'])
                gate = b / 'copied-gate.json'
                gate.write_text(json.dumps({'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1, 'implementation_sha256': snapshot['implementation_sha256'], 'candidate_sha256': snapshot['candidate_sha256'], 'configuration_sha256': first, 'case': snapshot['case'], 'case_sha256': snapshot['case_sha256'], 'image_id': snapshot['image_id']}))
                with self.assertRaisesRegex(ValueError, 'stale'):
                    launch.execute(Path('/nonexistent-auth'), gate)
                for name in ('jobs', 'harbor-t044-consumed'):
                    link = b / name
                    link.symlink_to(a / 'jobs', target_is_directory=True)
                    with self.assertRaisesRegex(ValueError, 'symlink alias'):
                        launch.plan()
                    with self.assertRaisesRegex(ValueError, 'symlink alias'):
                        launch.jobs_root()
                    link.unlink()

    def test_wrong_cwd_fails_before_inventory(self):
        import os
        with tempfile.TemporaryDirectory() as temp:
            old = Path.cwd()
            try:
                os.chdir(temp)
                with self.assertRaisesRegex(ValueError, 'repository root'):
                    launch.plan()
            finally:
                os.chdir(old)

    def test_mocked_mismatch_fails(self):
        with patch.object(launch, 'inventory', return_value={}):
            with self.assertRaisesRegex(ValueError, 'candidate'):
                launch.plan()

    def test_actual_candidate_inventory_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            package = Path(temp) / 'planar'
            marker = package / '.claude-plugin/plugin.json'
            marker.parent.mkdir(parents=True)
            marker.write_text('{}')
            with patch.object(launch, 'CANDIDATE', package), patch.object(launch, 'inventory', actual_inventory):
                with self.assertRaisesRegex(ValueError, 'candidate differs'):
                    launch.plan()

    def test_invalid_package_and_git_root_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(launch, 'CANDIDATE', root / 'missing'):
                with self.assertRaisesRegex(ValueError, 'package root'):
                    launch.plan()
            with patch.object(launch, 'ROOT', root), patch.object(launch.Path, 'cwd', return_value=root):
                with self.assertRaisesRegex(ValueError, 'git root'):
                    launch.plan()

    def test_write_destinations_reject_files(self):
        for name in ('harbor-t044-jobs', 'harbor-t044-consumed'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                reviews = Path(temp)
                (reviews / name).write_text('blocked')
                with patch.object(launch, 'REVIEWS', reviews), patch.object(launch, 'jobs_root', return_value=reviews / 'harbor-t044-jobs'):
                    with self.assertRaisesRegex(ValueError, 'not a directory'):
                        launch.plan()

    def test_unwritable_destination_fails(self):
        with patch.object(launch.os, 'access', return_value=False):
            with self.assertRaisesRegex(ValueError, 'not writable'):
                launch.plan()

    def test_plan_is_one_fresh_docker_attempt_with_explicit_config(self):
        with patch.object(launch.subprocess, 'run', side_effect=AssertionError('must not start a process')):
            plan = launch.plan()
        config = plan['configuration']
        self.assertEqual(config['n_attempts'], 1)
        self.assertEqual(config['n_concurrent_trials'], 1)
        self.assertEqual(config['environment']['import_path'], 'guarded_docker:GuardedDocker')
        self.assertEqual(config['agents'][0]['kwargs']['config'], launch.CONFIG)
        self.assertEqual(config['agents'][0]['kwargs']['version'], '0.169.0')
        self.assertEqual(config['agents'][0]['kwargs']['reasoning_effort'], 'medium')
        self.assertEqual(config['agents'][0]['skills'], [])
        self.assertEqual(config['agents'][0]['mcp_servers'], [])
        self.assertFalse(config['agents'][0]['resume_trajectory'])

    def test_lineage_case_is_explicit_and_image_bound(self):
        self.case_name = 'native-lineage-smoke'
        plan = launch.plan('native-lineage-smoke')
        self.assertEqual(plan['case'], 'native-lineage-smoke')
        self.assertEqual(plan['configuration']['tasks'][0]['path'], str(launch.CASES['native-lineage-smoke']))
        self.assertEqual(plan['case_sha256'], launch.case_inventory(launch.CASES['native-lineage-smoke']))
        self.assertEqual(plan['image_id'], 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7')

    def test_plan_and_execution_use_same_generated_settings(self):
        for name, case in launch.CASES.items():
            with self.subTest(case=name):
                self.case_name = name
                planned = launch.plan(name)['configuration']
                actual = launch.config_for('actual-run', Path('/tmp/actual-run'), case)
                for key in ('job_name', 'jobs_dir'):
                    planned.pop(key); actual.pop(key)
                self.assertEqual(planned, actual)

    def test_recovery_selection_matches_fixed_instruction_and_job_config(self):
        self.case_name = 'fresh-interrupted-review'
        original = launch.config_for
        for field, bad in (('model_name', 'wrong-model'), ('reasoning_effort', 'high')):
            def altered(*args, **kwargs):
                data = original(*args, **kwargs)
                if field == 'model_name': data['agents'][0]['model_name'] = bad
                else: data['agents'][0]['kwargs']['reasoning_effort'] = bad
                return data
            with self.subTest(field=field), patch.object(launch, 'config_for', side_effect=altered):
                with self.assertRaisesRegex(ValueError, 'session selection'):
                    launch.plan(self.case_name)

    def test_recovery_selection_rejects_missing_or_misattributed_source(self):
        from smoke_codex import RECOVERY_SESSION_SELECTION, validate_recovery_selection
        validate_recovery_selection(RECOVERY_SESSION_SELECTION, 'gpt-6.1-terra', 'medium')
        for instruction in ('', RECOVERY_SESSION_SELECTION.replace('launcher selected', 'backend verified'),
                            RECOVERY_SESSION_SELECTION + RECOVERY_SESSION_SELECTION):
            with self.subTest(instruction=instruction):
                with self.assertRaisesRegex(ValueError, 'session selection'):
                    validate_recovery_selection(instruction, 'gpt-6.1-terra', 'medium')

    def test_recovery_requires_gate_before_auth(self):
        self.case_name = 'fresh-interrupted-review'
        plan = launch.plan(self.case_name)
        self.assertIn('independent review', plan['prerequisites']['execution_gate'])
        with patch.object(launch.subprocess, 'run', side_effect=AssertionError('must not start a process')):
            with self.assertRaises(FileNotFoundError):
                launch.execute(Path('/missing/auth'), Path('/missing/review'), self.case_name)

    def test_lineage_stale_case_gate_fails_before_auth(self):
        self.case_name = 'native-lineage-smoke'
        plan = launch.plan('native-lineage-smoke')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            review = root / 'review.json'
            gate = {**plan, 'verdict': 'approve', 'max_attempts': 1, 'case_sha256': 'stale'}
            review.write_text(json.dumps(gate))
            with patch.object(launch, 'REVIEWS', root), patch.object(launch.subprocess, 'run', side_effect=AssertionError('must not start a process')):
                with self.assertRaisesRegex(ValueError, 'stale'):
                    launch.execute(root / 'missing-auth', review, 'native-lineage-smoke')

    def test_stale_review_fails_before_auth_access_or_process_start(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            review = root / 'review.json'
            review.write_text(json.dumps({'verdict': 'approve', 'implementation_sha256': 'stale'}))
            with patch.object(launch, 'REVIEWS', root), patch.object(launch.subprocess, 'run', side_effect=AssertionError('must not start a process')):
                with self.assertRaisesRegex(ValueError, 'stale'):
                    launch.execute(root / 'missing-auth', review)

    def test_review_consumption_is_one_use(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            review = root / 'review.json'
            review.write_text('{"purpose":"one-diagnostic-attempt"}')
            first = launch.consume_review_once(review, root / 'consumed')
            self.assertEqual(len(first), 64)
            with self.assertRaisesRegex(ValueError, 'already consumed'):
                launch.consume_review_once(review, root / 'consumed')
