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
                with patch.object(launch, 'REVIEWS', reviews):
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
