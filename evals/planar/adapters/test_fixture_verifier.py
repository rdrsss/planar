import importlib.util
import tempfile
import unittest
from pathlib import Path

path = Path(__file__).resolve().parents[1] / 'cases/plugin-smoke/tests/verify.py'
spec = importlib.util.spec_from_file_location('smoke_verify', path)
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class ResultControls(unittest.TestCase):
    def test_absent_wrong_extra_and_symlink_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = root / 'RESULT.txt'
            self.assertIsNotNone(verify.result_check(root))
            result.write_bytes(b'2')
            self.assertIsNotNone(verify.result_check(root))
            result.write_bytes(b'2\n')
            self.assertIsNone(verify.result_check(root))
            (root / 'unexpected').write_bytes(b'')
            self.assertIsNotNone(verify.result_check(root))
            (root / 'unexpected').unlink()
            result.unlink()
            result.symlink_to('/etc/passwd')
            self.assertIsNotNone(verify.result_check(root))

    def test_verifier_exception_is_infrastructure_error(self):
        from unittest.mock import patch
        with patch.object(verify, 'result_check', side_effect=OSError('failure')):
            self.assertEqual(verify.main(), 2)

    def test_complete_result_alone_is_not_a_smoke_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'RESULT.txt').write_bytes(b'2\n')
            from unittest.mock import patch
            with patch.object(verify, 'Path', return_value=root):
                self.assertEqual(verify.main(), 2)

class SyntheticDispatchControls(unittest.TestCase):
    def make_events(self):
        import hashlib
        import json
        root = Path(__file__).resolve().parents[3] / 'plugins/planar'
        base = '/installed-agent/codex-home/plugins/cache/planar-smoke-market/planar/0.1.0/'
        files = ['agents/worker.md', 'references/hosts.md', 'skills/run/SKILL.md']
        content = {base + name: (root / name).read_text() for name in files}
        hashes = {name: hashlib.sha256(text.encode()).hexdigest() for name, text in content.items()}
        events = [{'type': 'session_meta', 'payload': {'id': 'root'}}]
        for index, (name, text) in enumerate(content.items()):
            call = str(index)
            events.extend([
                {'type': 'response_item', 'payload': {'type': 'function_call', 'name': 'exec_command', 'call_id': call, 'arguments': json.dumps({'cmd': 'cat ' + name})}},
                {'type': 'response_item', 'payload': {'type': 'function_call_output', 'call_id': call, 'output': text}},
            ])
        role = content[base + 'agents/worker.md']
        events.append({'type': 'response_item', 'payload': {'type': 'function_call', 'name': 'spawn_agent', 'call_id': 'spawn', 'arguments': json.dumps({'message': role + '\nPacket: write /workspace/RESULT.txt', 'fork_turns': 'none', 'model': 'gpt-6.1-terra', 'reasoning_effort': 'medium'})}})
        events.append({'type': 'response_item', 'payload': {'type': 'function_call_output', 'call_id': 'spawn', 'output': json.dumps({'task_name': '/root/worker'})}})
        child = [
            {'type': 'session_meta', 'payload': {'id': 'child', 'source': {'subagent': {'thread_spawn': {'parent_thread_id': 'root', 'agent_path': '/root/worker'}}}}},
            {'type': 'response_item', 'payload': {'type': 'function_call', 'name': 'exec_command', 'arguments': '{"cmd":"write /workspace/RESULT.txt"}'}},
            {'type': 'event_msg', 'payload': {'type': 'task_complete'}},
        ]
        return events, child, hashes

    def test_complete_synthetic_passes_structural_check(self):
        self.assertTrue(verify.structural_dispatch(*self.make_events()))

    def test_claimed_dispatch_incomplete_role_and_mismatch_fail(self):
        import json
        events, child, hashes = self.make_events()
        self.assertFalse(verify.structural_dispatch([events[0], events[-1]], child, hashes))
        args = json.loads(events[-2]['payload']['arguments'])
        args['message'] = 'worker role was provided'
        events[-2]['payload']['arguments'] = json.dumps(args)
        self.assertFalse(verify.structural_dispatch(events, child, hashes))
        events, child, hashes = self.make_events()
        hashes[next(iter(hashes))] = '0' * 64
        self.assertFalse(verify.structural_dispatch(events, child, hashes))
        events, child, hashes = self.make_events()
        child[0]['payload']['id'] = 'root'
        self.assertFalse(verify.structural_dispatch(events, child, hashes))
