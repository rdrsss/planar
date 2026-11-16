import json
import logging
import re
import unittest
import tempfile
from pathlib import Path
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from harbor.agents.installed.codex import Codex
import native_status


class NativeStatusTest(unittest.TestCase):
    def test_pinned_codex_filters_model_and_tool_events(self):
        codex = Codex.__new__(Codex)
        codex._compiled_error_patterns = [(re.compile(p.pattern, re.IGNORECASE), p.exception) for p in codex.ERROR_PATTERNS]
        codex.logger = logging.getLogger('native-control-test')
        codex.options = None
        def classified(event):
            result = SimpleNamespace(return_code=1, stdout=json.dumps(event), stderr='')
            return type(codex._classify_exec_error('codex exec --json', result)).__name__
        self.assertEqual(classified({'type': 'turn.failed', 'error': { 'message': "You've hit your usage limit"}}), 'ApiUsageLimitError')
        self.assertNotEqual(classified({'type': 'item.completed', 'item': {'text': "You've hit your usage limit"}}), 'ApiUsageLimitError')
        self.assertNotEqual(classified({'type': 'item.completed', 'item': {'type': 'command_execution', 'output': "You've hit your usage limit"}}), 'ApiUsageLimitError')

    def test_only_native_typed_usage_limit_stops(self):
        def result(*names):
            return SimpleNamespace(recorded_exceptions=lambda: [SimpleNamespace(exception_type=n) for n in names])
        self.assertTrue(native_status.classify(result('ApiUsageLimitError'))['stop_account'])
        self.assertEqual(native_status.classify(result('ApiUsageLimitError'))['status'], 'incomplete')
        self.assertEqual(native_status.classify(result())['status'], 'incomplete')
        self.assertEqual(native_status.classify(result('AgentAuthenticationError'))['status'], 'error')

    def test_binary_rewards_require_consistent_separate_verifier_evidence(self):
        now = datetime.now(timezone.utc)
        def result(value, errors=()):
            return SimpleNamespace(recorded_exceptions=lambda: [SimpleNamespace(exception_type=e) for e in errors],
                config=SimpleNamespace(verifier=SimpleNamespace(disable=False)), verifier_environment_mode='separate',
                step_results=None, reward_valid=True, started_at=now, finished_at=now + timedelta(seconds=3),
                verifier=SimpleNamespace(started_at=now + timedelta(seconds=1), finished_at=now + timedelta(seconds=2)),
                verifier_result=SimpleNamespace(rewards={'reward': value}))
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            status, reward = folder / 'status.json', folder / 'reward.txt'
            for value, label in ((1, 'pass'), (0, 'behavioral_failure')):
                status.write_text(json.dumps({'status': label}))
                reward.write_text(str(value))
                self.assertEqual(native_status.assess_verified(result(value), folder)['status'], label)
            status.write_text(json.dumps({'status': 'infrastructure_error'}))
            self.assertEqual(native_status.assess_verified(result(0), folder)['status'], 'incomplete')
            status.write_text(json.dumps({'status': 'pass'}))
            self.assertEqual(native_status.assess_verified(result(0), folder)['status'], 'incomplete')
            self.assertEqual(native_status.assess_verified(result(1, ('ApiUsageLimitError',)), folder)['reason'], 'native usage limit')
            reward.unlink()
            self.assertEqual(native_status.assess_verified(result(1), folder)['status'], 'incomplete')
            reward.write_text('1')
            status.write_text('{"status":"pass", "status":"behavioral_failure"}')
            self.assertEqual(native_status.assess_verified(result(1), folder)['status'], 'incomplete')


if __name__ == '__main__':
    unittest.main()
