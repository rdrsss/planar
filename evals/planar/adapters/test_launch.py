import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import launch


class LaunchControls(unittest.TestCase):
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
        plan = launch.plan('native-lineage-smoke')
        self.assertEqual(plan['case'], 'native-lineage-smoke')
        self.assertEqual(plan['configuration']['tasks'][0]['path'], str(launch.CASES['native-lineage-smoke']))
        self.assertEqual(plan['case_sha256'], launch.case_inventory(launch.CASES['native-lineage-smoke']))
        self.assertEqual(plan['image_id'], 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7')

    def test_lineage_stale_case_gate_fails_before_auth(self):
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
