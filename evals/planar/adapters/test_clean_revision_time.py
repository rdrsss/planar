"""No-auth controls: an insufficient B budget cannot inject or dispatch."""
import asyncio
from datetime import datetime, timedelta, timezone
import json
import time
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch
from harbor.agents.installed.codex import Codex
from smoke_codex import SmokeCodex


class TimeBoundaryTests(unittest.TestCase):
    def evidence(self, root, *, elapsed, expiry_seconds):
        now = time.monotonic_ns()
        absolute = (datetime.now(timezone.utc)+timedelta(seconds=expiry_seconds)).isoformat()
        (root/'harbor-process.json').write_text(json.dumps({'execution_id':root.name, 'started_monotonic_ns':now-int(elapsed*1e9), 'deadline_seconds':1050, 'absolute_deadline':absolute}))
        return {'PLANAR_T044_CASE_SNAPSHOT':'/frozen', 'PLANAR_T044_CANDIDATE_SNAPSHOT':'/frozen',
                'PLANAR_T044_ABSOLUTE_DEADLINE':absolute, 'PLANAR_T044_DEADLINE_SECONDS':'1050'}

    def test_insufficient_job_or_expiry_stops_before_injection(self):
        for elapsed, expiry in ((500,3600), (0,564), (0,-1)):
            with self.subTest(elapsed=elapsed, expiry=expiry), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp).resolve(); (root/'clean-revision').mkdir()
                agent = SmokeCodex.__new__(SmokeCodex); agent._case=root; agent._provenance_dir=root
                agent.model_name='gpt-6.1-terra'; agent.options=SimpleNamespace(reasoning_effort='medium')
                agent._clean_step='worker'; agent._clean_container='a'*64; agent._clean_baseline=['baseline']
                identity=SimpleNamespace(load_instructions=lambda _: {},validate_selection=lambda *_:None,identify=lambda *_:'revision')
                handoff=SimpleNamespace(worker_gate=Mock())
                capture=SimpleNamespace(load=Mock(return_value={'state':b'before'}), collect=AsyncMock(return_value={'state':b'before'}), inject=AsyncMock(), save=Mock())
                def load(_, name): return {'step_identity':identity,'handoff':handoff,'capture':capture}[name]
                env=self.evidence(root,elapsed=elapsed,expiry_seconds=expiry)
                with patch.dict('os.environ',env,clear=True), patch('clean_revision_support.load_case_module',side_effect=load):
                    with self.assertRaisesRegex(TimeoutError,'insufficient time'):
                        asyncio.run(agent._prepare_clean_step('revision',object()))
                capture.inject.assert_not_awaited(); capture.save.assert_not_called()
                self.assertEqual(agent._clean_step,'worker')
                self.assertFalse((root/'clean-revision/post').exists())

    def test_expired_or_near_expiry_never_dispatches(self):
        for elapsed, expiry in ((0,-1), (0,474), (576,3600)):
            with self.subTest(elapsed=elapsed, expiry=expiry), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp).resolve(); agent=SmokeCodex.__new__(SmokeCodex)
                agent._case_name='clean-revision'; agent._clean_step='revision'; agent._provenance_dir=root
                env=self.evidence(root,elapsed=elapsed,expiry_seconds=expiry)
                dispatched=AsyncMock()
                with patch.dict('os.environ',env,clear=True), patch.object(Codex,'exec_as_agent',new=dispatched):
                    with self.assertRaisesRegex(TimeoutError,'insufficient time'):
                        asyncio.run(agent.exec_as_agent(object(),command='codex exec --help'))
                dispatched.assert_not_awaited()

if __name__=='__main__': unittest.main()
