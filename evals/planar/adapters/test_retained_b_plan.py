import os
import json
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import launch

class RetainedPlanControls(unittest.TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        root=Path(self.tmp.name); self.packet=root/'packet'; self.packet.mkdir()
        self.env=patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}); self.env.start(); self.addCleanup(self.env.stop)
        self.source=patch('retained_b_support.source',return_value={'source_packet_sha256':'a'*64,'assessment':{'status':'pass'}}); self.source.start(); self.addCleanup(self.source.stop)
        self.inventory=patch('launch.inventory',return_value=json.loads((launch.EVALS/'planar/cases/retained-a-fresh-b'/'candidate.json').read_text())); self.inventory.start(); self.addCleanup(self.inventory.stop)
        self.kw={'candidate_root':launch.ROOT/'plugins/planar','case_root':launch.EVALS/'planar/cases/retained-a-fresh-b',
                 'source_packet':self.packet,'trial_deadline':1050,'execution_id':'planar-t044-'+'1'*32,'absolute_deadline':'2100-01-01T00:00:00+00:00'}
    def test_reserved_id_and_effective_config_are_bound(self):
        a=launch.plan('retained-a-fresh-b',**self.kw)
        self.kw['execution_id']='planar-t044-'+'2'*32; b=launch.plan('retained-a-fresh-b',**self.kw)
        self.assertNotEqual(a['configuration_sha256'],b['configuration_sha256'])
        self.assertNotEqual(a['prerequisites']['effective_configuration_sha256'],b['prerequisites']['effective_configuration_sha256'])
        self.assertEqual(a['prerequisites']['source_packet_sha256'],'a'*64)
        self.assertEqual(a['prerequisites']['execution_gate'],'requires an independently reviewed, exact source-bound one-use gate before authentication')
    def test_missing_source_id_expiry_or_budget_fail(self):
        for key,value in (('source_packet',None),('execution_id',None),('absolute_deadline',None),('trial_deadline',1049)):
            args=dict(self.kw); args[key]=value
            with self.subTest(key=key), self.assertRaises(ValueError): launch.plan('retained-a-fresh-b',**args)
    def test_source_packet_is_not_an_old_case_bypass(self):
        with self.assertRaises(ValueError): launch.plan('clean-revision',source_packet=self.packet)
    def test_pending_account_refuses_before_auth_or_gate(self):
        import codex_run
        from contextlib import contextmanager
        @contextmanager
        def admission():
            yield Path(self.tmp.name)
        with patch.object(codex_run, 'admission_scope', admission), patch.object(codex_run, 'shared_pending', return_value=True), patch.object(launch, '_execute_inner', side_effect=AssertionError('gate or auth accessed')):
            with self.assertRaisesRegex(RuntimeError, 'account admission blocked'):
                launch.execute(self.packet, self.packet, 'retained-a-fresh-b', **self.kw)

    def test_consumes_gate_and_records_account_before_auth_access(self):
        from types import SimpleNamespace
        from launch import Task
        snapshot=launch.plan('retained-a-fresh-b',**self.kw)
        case=self.kw['case_root']; task=Task(case)
        gate={key:snapshot[key] for key in ('implementation_sha256','candidate_sha256','configuration_sha256','case','case_sha256','image_id')}
        gate['selection_context']=snapshot['prerequisites']['selection_context']
        gate.update(verdict='approve',purpose='one-diagnostic-attempt',max_attempts=1,
                    verifier_image_id=task.config.verifier.environment.docker_image)
        review=Path(self.tmp.name)/'gate.json'; review.write_text(json.dumps(gate))
        records=Path(self.tmp.name)/'planar-account-admission-v1'/'records'; records.mkdir(parents=True)
        real_run=subprocess.run
        def inspect(args, **kwargs):
            if args[0]=='git': return real_run(args,**kwargs)
            if args[:3] != ['docker','image','inspect']: raise AssertionError('unexpected subprocess')
            return SimpleNamespace(returncode=0,stdout=args[-1]+'\n')
        with patch('retained_b_support.validate_gate'), patch.object(launch.shutil,'which',return_value='/usr/bin/docker'), patch.object(launch.subprocess,'run',side_effect=inspect):
            with self.assertRaises(FileNotFoundError):
                launch._execute_inner(Path('/definitely-absent-planar-auth'),review,'retained-a-fresh-b',_retained_records=records,**self.kw)
        self.assertTrue((records/(self.kw['execution_id']+'.start.json')).is_file())
        self.assertEqual(len(list((Path(self.tmp.name)/'harbor-t044-consumed').iterdir())),1)

    def test_swap_during_source_validation_never_consumes_or_reads_auth(self):
        from types import SimpleNamespace
        snapshot=launch.plan('retained-a-fresh-b',**self.kw); prereq=snapshot['prerequisites']
        policy=json.loads((self.kw['case_root']/'source-policy.json').read_text()); task=launch.Task(self.kw['case_root'])
        gate={key:snapshot[key] for key in ('implementation_sha256','candidate_sha256','configuration_sha256','case','case_sha256','image_id')}
        gate['selection_context']=prereq['selection_context']
        gate.update(verdict='approve',purpose='one-diagnostic-attempt',max_attempts=1,execution_id=self.kw['execution_id'],absolute_deadline=self.kw['absolute_deadline'],effective_configuration_sha256=prereq['effective_configuration_sha256'],source_packet_sha256=prereq['source_packet_sha256'],source_packet_root=prereq['source_packet_root'],source_execution_id=policy['execution'],source_online_status='behavioral_failure',source_online_reward=0,source_assessor_image=policy['assessor_image'],source_assessment_sha256=policy['files']['evidence/corrected-assessment.txt']['sha256'],source_semantic_audit_sha256=policy['files']['evidence/semantic-audit.md']['sha256'],verifier_image_id=task.config.verifier.environment.docker_image,account_scope_id=str(Path(self.tmp.name)/'planar-account-admission-v1'),runtime_budget_seconds=1050,step_agent_seconds=400,step_verifier_seconds=60,cleanup_grace_seconds=15,result_boundary='incomplete-pending-independent-bound-raw-audit',auth_path='/definitely-absent-planar-auth')
        review=Path(self.tmp.name)/'gate.json'; review.write_text(json.dumps(gate)); records=Path(self.tmp.name)/'planar-account-admission-v1'/'records'; records.mkdir(parents=True)
        count=0
        def source(*args):
            nonlocal count
            count+=1
            if count==2: review.write_text(json.dumps({**gate,'purpose':'changed'}))
            return {'source_packet_sha256':'a'*64,'assessment':{'status':'pass'}}
        real_run=subprocess.run
        def inspect(args, **kwargs):
            if args[0]=='git': return real_run(args,**kwargs)
            if args[:3] != ['docker','image','inspect']: raise AssertionError('unexpected subprocess')
            return SimpleNamespace(returncode=0,stdout=args[-1]+'\n')
        with patch('retained_b_support.source',side_effect=source), patch.object(launch.shutil,'which',return_value='/usr/bin/docker'), patch.object(launch.subprocess,'run',side_effect=inspect):
            with self.assertRaisesRegex(ValueError,'changed before consumption'):
                launch._execute_inner(Path(gate['auth_path']),review,'retained-a-fresh-b',_retained_records=records,**self.kw)
        self.assertFalse((Path(self.tmp.name)/'harbor-t044-consumed').exists())
        self.assertEqual(list(records.iterdir()),[])

if __name__=='__main__': unittest.main()
