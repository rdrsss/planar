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
