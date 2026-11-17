"""No-auth tests for clean-revision host step preconditions."""
import asyncio
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import AsyncMock, Mock, patch
from smoke_codex import SmokeCodex

class StepTests(unittest.TestCase):
    def test_revision_requires_archived_gate_then_unchanged_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'clean-revision').mkdir()
            agent = SmokeCodex.__new__(SmokeCodex)
            agent._case = root; agent._provenance_dir = root
            agent._clean_step = 'worker'; agent._clean_container = 'a'*64; agent._clean_baseline = ['baseline']
            identity = SimpleNamespace(load_instructions=lambda _: {}, identify=lambda *_:'revision')
            handoff = SimpleNamespace(worker_gate=Mock(side_effect=ValueError('gate missing')))
            capture = SimpleNamespace(load=Mock(), collect=AsyncMock(), inject=AsyncMock(), save=Mock())
            def load(_, name): return {'step_identity':identity,'handoff':handoff,'capture':capture}[name]
            env = {'PLANAR_T044_CASE_SNAPSHOT':'/frozen', 'PLANAR_T044_CANDIDATE_SNAPSHOT':'/frozen'}
            with patch.dict('os.environ',env), patch('clean_revision_support.load_case_module',side_effect=load), patch('clean_revision_support.check_revision_time'):
                with self.assertRaisesRegex(ValueError,'gate missing'):
                    asyncio.run(agent._prepare_clean_step('revision',object()))
                capture.inject.assert_not_called()
                handoff.worker_gate.side_effect = None
                capture.load.return_value = {'state':b'before'}
                capture.collect.return_value = {'state':b'changed'}
                with self.assertRaisesRegex(ValueError,'snapshot changed'):
                    asyncio.run(agent._prepare_clean_step('revision',object()))
                capture.inject.assert_not_called()
                capture.collect.return_value = {'state':b'before'}
                capture.inject.return_value = ({'state':b'after'}, {'changed':'STATE'})
                asyncio.run(agent._prepare_clean_step('revision',object()))
                self.assertEqual(agent._clean_step,'revision')
                capture.save.assert_called_once()
                capture.inject.assert_awaited_once()

if __name__ == '__main__': unittest.main()
