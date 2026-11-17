import json
import os
import hashlib
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch
import launch

class CleanWorkGate(TestCase):
    def test_exact_scope_and_consumption_precede_auth(self):
        image='sha256:'+'a'*64; eid='planar-t044-'+'1'*32
        snapshot={'implementation_sha256':'impl','candidate_sha256':'candidate','configuration_sha256':'config','case_sha256':'case','image_id':image}
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); records=root/'planar-account-admission-v1/records'; records.mkdir(parents=True); case=root/'case'; case.mkdir(); jobs=root/'jobs'; jobs.mkdir(); review=root/'review.json'
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): ctx=launch.context()
            effective=launch.config_for(eid,jobs/eid,case,'medium'); digest=hashlib.sha256(json.dumps(effective,sort_keys=True,separators=(',',':')).encode()).hexdigest(); snapshot['prerequisites']={'effective_configuration_sha256':digest,'selection_context':ctx.identity()}
            gate={'selection_context':ctx.identity(),'effective_configuration_sha256':digest,'verdict':'approve','purpose':'one-diagnostic-attempt','max_attempts':1,**snapshot,'case':'clean-work-review','model_name':'gpt-6.1-terra','reasoning_effort':'medium','execution_id':eid,'account_scope_id':str(records.parent),'verifier_image_id':image,'absolute_deadline':'2100-01-01T00:00:00+00:00'}; review.write_text(json.dumps(gate)); task=SimpleNamespace(config=SimpleNamespace(environment=SimpleNamespace(docker_image=image),verifier=SimpleNamespace(environment=SimpleNamespace(docker_image=image))))
            with patch.object(launch,'context',return_value=ctx),patch.object(launch,'plan',return_value=snapshot),patch.object(launch,'reviews_root',return_value=root),patch.object(launch,'jobs_root',return_value=jobs),patch.object(launch,'Task',return_value=task),patch.object(launch.shutil,'which',return_value='/usr/bin/docker'),patch.object(launch.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=image+'\n')):
                with self.assertRaises(FileNotFoundError): launch._execute_inner(root/'missing-auth',review,'clean-work-review',case_root=case,candidate_root=root,trial_deadline=1050,absolute_deadline=gate['absolute_deadline'],execution_id=eid,_retained_records=records)
            self.assertEqual(len(list((root/'harbor-t044-consumed').iterdir())),1); self.assertEqual(json.loads((records/(eid+'.start.json')).read_text())['status'],'awaiting-independent-bound-raw-audit'); self.assertEqual(json.loads((records/(eid+'.start.json')).read_text())['gate_authority'],gate)
    def test_effective_config_mismatch_stops_before_consumption(self):
        image='sha256:'+'a'*64; eid='planar-t044-'+'2'*32
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); records=root/'planar-account-admission-v1/records'; records.mkdir(parents=True); case=root/'case'; case.mkdir(); jobs=root/'jobs'; jobs.mkdir(); review=root/'review.json'
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): ctx=launch.context()
            snapshot={'implementation_sha256':'impl','candidate_sha256':'candidate','configuration_sha256':'config','case_sha256':'case','image_id':image,'prerequisites':{'effective_configuration_sha256':'a'*64,'selection_context':ctx.identity()}}
            gate={'selection_context':ctx.identity(),'verdict':'approve','purpose':'one-diagnostic-attempt','max_attempts':1,**snapshot,'case':'clean-work-review','model_name':'gpt-6.1-terra','reasoning_effort':'medium','execution_id':eid,'account_scope_id':str(records.parent),'effective_configuration_sha256':'b'*64,'verifier_image_id':image,'absolute_deadline':'2100-01-01T00:00:00+00:00'}; review.write_text(json.dumps(gate))
            with patch.object(launch,'plan',return_value=snapshot),patch.object(launch,'reviews_root',return_value=root),patch.object(launch,'jobs_root',return_value=jobs):
                with self.assertRaisesRegex(ValueError,'effective JobConfig mismatch'): launch._execute_inner(root/'missing-auth',review,'clean-work-review',case_root=case,candidate_root=root,trial_deadline=1050,absolute_deadline=gate['absolute_deadline'],execution_id=eid,_retained_records=records)
            self.assertFalse((root/'harbor-t044-consumed').exists()); self.assertFalse(list(records.iterdir()))

    def test_duplicate_gate_keys_rejected_before_consumption(self):
        eid='planar-t044-'+'3'*32
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); records=root/'records'; records.mkdir(); review=root/'review.json'
            review.write_text('{"verdict":"approve","verdict":"approve"}')
            with patch.object(launch,'plan',return_value={}),patch.object(launch,'reviews_root',return_value=root):
                with self.assertRaises(ValueError): launch._execute_inner(root/'missing-auth',review,'clean-work-review',case_root=root,candidate_root=root,trial_deadline=1050,absolute_deadline='2100-01-01T00:00:00+00:00',execution_id=eid,_retained_records=records)
            self.assertFalse((root/'harbor-t044-consumed').exists()); self.assertFalse(list(records.iterdir()))

    def test_account_pending_blocks_before_inner_launch(self):
        sys.path.insert(0,str(launch.EVALS/'planar/experiments'))
        self.addCleanup(sys.path.remove,str(launch.EVALS/'planar/experiments'))
        import codex_run
        from contextlib import contextmanager
        @contextmanager
        def locked(): yield Path('/nonexistent')
        with patch.object(codex_run,'admission_scope',locked),patch.object(codex_run,'shared_pending',return_value=True),patch.object(launch,'_execute_inner',side_effect=AssertionError('must not dispatch')):
            with self.assertRaisesRegex(RuntimeError,'account admission blocked'): launch.execute(Path('/missing-auth'),Path('/missing-gate'),'clean-work-review')

    def test_unreviewed_marker_cannot_clear_clean_work_account(self):
        from tempfile import TemporaryDirectory
        sys.path.insert(0,str(launch.EVALS/'planar/experiments')); self.addCleanup(sys.path.remove,str(launch.EVALS/'planar/experiments'))
        import codex_run
        with TemporaryDirectory() as tmp:
            records=Path(tmp); (records/'trial.start.json').write_text(json.dumps({'case':'clean-work-review'})); (records/'trial.resolved.json').write_text('{}')
            self.assertEqual(len(codex_run.shared_pending(records)),1)
