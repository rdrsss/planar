import asyncio
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import AsyncMock,Mock,patch
from smoke_codex import SmokeCodex

class CleanWorkSteps(TestCase):
    def test_review_requires_worker_gate_and_never_injects(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp); target=root/'clean-revision'; target.mkdir(); (target/'pre-receipt.json').write_text(json.dumps({'manifest_sha256':'a'*64}))
            a=SmokeCodex.__new__(SmokeCodex); a._case=root; a._case_name='clean-work-review'; a._provenance_dir=root; a.model_name='gpt-6.1-terra'; a.options=SimpleNamespace(reasoning_effort='medium'); a._clean_step='worker'; a._clean_container='a'*64; a._clean_baseline=['baseline']
            ident=SimpleNamespace(load_instructions=lambda _: {},validate_selection=lambda *_:None,identify=lambda *_:'review'); gate=SimpleNamespace(worker_gate=Mock(side_effect=ValueError('gate missing'))); data={'repository/RESULT.txt':b'2\n','repository/STATE.txt':b'correct\n'}; capture=SimpleNamespace(load=Mock(return_value=data),collect=AsyncMock(return_value=data),save=Mock(),inject=AsyncMock()); nofault=SimpleNamespace(receipt=Mock(return_value={'mode':'none'}))
            def load(_,name): return {'step_identity':ident,'handoff':gate,'capture':capture,'no_fault':nofault}[name]
            with patch.dict('os.environ',{'PLANAR_T044_CASE_SNAPSHOT':'/frozen','PLANAR_T044_CANDIDATE_SNAPSHOT':'/frozen'}),patch('clean_revision_support.load_case_module',side_effect=load),patch('clean_revision_support.check_revision_time'):
                with self.assertRaisesRegex(ValueError,'gate missing'): asyncio.run(a._prepare_clean_step('review',object()))
                capture.collect.assert_not_awaited(); capture.inject.assert_not_awaited()
                gate.worker_gate.side_effect=None; capture.collect.return_value={**data,'repository/STATE.txt':b'changed'}
                with self.assertRaisesRegex(ValueError,'snapshot changed'): asyncio.run(a._prepare_clean_step('review',object()))
                capture.inject.assert_not_awaited(); capture.collect.return_value=data
                asyncio.run(a._prepare_clean_step('review',object()))
                self.assertEqual(a._clean_step,'review'); capture.inject.assert_not_awaited(); self.assertEqual(json.loads((target/'no-fault.json').read_text())['mode'],'none')
                self.assertEqual(json.loads((target/'worker-gate.json').read_text()),{'reward':'1','pre_manifest_sha256':'a'*64})
    def test_expired_review_never_writes_no_fault_receipt(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'clean-revision').mkdir()
            a=SmokeCodex.__new__(SmokeCodex); a._case=root; a._case_name='clean-work-review'; a._provenance_dir=root; a.model_name='gpt-6.1-terra'; a.options=SimpleNamespace(reasoning_effort='medium'); a._clean_step='worker'; a._clean_container='a'*64; a._clean_baseline=['baseline']
            ident=SimpleNamespace(load_instructions=lambda _: {},validate_selection=lambda *_:None,identify=lambda *_:'review'); gate=SimpleNamespace(worker_gate=Mock()); capture=SimpleNamespace(load=Mock(return_value={'x':b'y'}),collect=AsyncMock(return_value={'x':b'y'}),save=Mock(),inject=AsyncMock()); nofault=SimpleNamespace(receipt=Mock())
            def load(_,name): return {'step_identity':ident,'handoff':gate,'capture':capture,'no_fault':nofault}[name]
            with patch.dict('os.environ',{'PLANAR_T044_CASE_SNAPSHOT':'/frozen','PLANAR_T044_CANDIDATE_SNAPSHOT':'/frozen'}),patch('clean_revision_support.load_case_module',side_effect=load),patch('clean_revision_support.check_revision_time',side_effect=TimeoutError('expired')):
                with self.assertRaises(TimeoutError): asyncio.run(a._prepare_clean_step('review',object()))
                nofault.receipt.assert_not_called(); capture.save.assert_not_called(); capture.inject.assert_not_awaited()
    def test_trusted_instruction_selection_and_confirmation(self):
        from clean_revision_support import load_case_module
        case=Path(__file__).resolve().parents[1]/'cases/clean-work-review'; identity=load_case_module(case,'step_identity'); frozen=identity.load_instructions(case)
        self.assertEqual(identity.identify(frozen['review'][0],frozen),'review')
        self.assertEqual(frozen['review'][0].count('Operator confirmation: the previous A driver and all its children have stopped; individual thread close was unsupported.'),1)
        with self.assertRaises(ValueError): identity.identify(frozen['review'][0]+' ',frozen)
        with self.assertRaises(ValueError): identity.validate_selection(frozen,'gpt-6.1-sol','medium')
