import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import launch


class Admission(unittest.TestCase):
    def fixture(self,root):
        eid='planar-t044-'+'1'*32; image='sha256:'+'a'*64; records=root/'planar-account-admission-v1/records';records.mkdir(parents=True)
        case=root/'case';case.mkdir(); jobs=root/'jobs';jobs.mkdir(); packet=root/'packet';packet.mkdir(); review=root/'review.json'
        policy={'execution':'source','assessor_image':'source-image','worker_role_delivery':'unverified','source_semantics':'packet-read-causal-write-completion-only','files':{'evidence/semantic-audit.md':{'sha256':'audit'},'evidence/offline-regrade.json':{'sha256':'regrade'}}}; (case/'source-policy.json').write_text(json.dumps(policy))
        effective=launch.config_for(eid,jobs/eid,case,'medium'); digest=hashlib.sha256(json.dumps(effective,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        snapshot={'implementation_sha256':'impl','candidate_sha256':'candidate','configuration_sha256':'config','case_sha256':'case','image_id':image,'prerequisites':{'effective_configuration_sha256':digest,'source_packet_sha256':'packet','source_packet_root':str(packet),'source_account_scope_id':str(root/'original/planar-account-admission-v1')}}
        expiry='2100-01-01T00:00:00+00:00'; gate={'verdict':'approve','purpose':'one-diagnostic-attempt','case':'clean-retained-a-continuation','max_attempts':1,'execution_id':eid,'absolute_deadline':expiry,'candidate_sha256':'candidate','case_sha256':'case','implementation_sha256':'impl','configuration_sha256':'config','effective_configuration_sha256':digest,'source_packet_sha256':'packet','source_packet_root':str(packet),'source_account_scope_id':snapshot['prerequisites']['source_account_scope_id'],'source_execution_id':'source','source_online_status':'behavioral_failure','source_online_reward':0,'source_assessor_image':'source-image','source_assessment_sha256':'regrade','source_semantic_audit_sha256':'audit','worker_role_delivery':'unverified','source_semantics':'packet-read-causal-write-completion-only','handoff':'no-fault','model_name':'gpt-6.1-terra','reasoning_effort':'medium','image_id':image,'verifier_image_id':image,'account_scope_id':str(records.parent),'runtime_budget_seconds':1050,'step_agent_seconds':400,'step_verifier_seconds':60,'cleanup_grace_seconds':15,'result_boundary':'incomplete-pending-independent-bound-raw-audit','auth_path':str(root/'missing-auth')}
        review.write_text(json.dumps(gate)); task=SimpleNamespace(config=SimpleNamespace(environment=SimpleNamespace(docker_image=image),verifier=SimpleNamespace(environment=SimpleNamespace(docker_image=image))))
        return eid,image,records,case,jobs,packet,review,snapshot,gate,task

    def run_admission(self,root,fixture):
        eid,image,records,case,jobs,packet,review,snapshot,gate,task=fixture
        account=SimpleNamespace(check=lambda *args:None)
        with patch.object(launch,'plan',return_value=snapshot),patch.object(launch,'reviews_root',return_value=root),patch.object(launch,'jobs_root',return_value=jobs),patch.object(launch,'Task',return_value=task),patch.object(launch.shutil,'which',return_value='/usr/bin/docker'),patch.object(launch.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=image+'\n')),patch('clean_revision_support.load_case_module',return_value=account),patch('retained_b_support.source',return_value={'source_packet_sha256':'packet'}):
            return launch._execute_inner(root/'missing-auth',review,'clean-retained-a-continuation',case_root=case,candidate_root=root,source_packet=packet,trial_deadline=1050,absolute_deadline=gate['absolute_deadline'],execution_id=eid,_retained_records=records)

    def test_consumes_exact_gate_before_auth(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); f=self.fixture(root)
            with self.assertRaises(FileNotFoundError): self.run_admission(root,f)
            self.assertEqual(len(list((root/'harbor-t044-consumed').iterdir())),1)
            start=json.loads((f[2]/(f[0]+'.start.json')).read_text()); self.assertEqual(start['gate_authority'],f[8]); self.assertEqual(start['case'],'clean-retained-a-continuation')

    def test_role_claim_rejected_before_consumption_and_auth(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); f=self.fixture(root); gate=f[8]; gate['worker_role_delivery']='verified'; f[6].write_text(json.dumps(gate))
            with self.assertRaises(ValueError): self.run_admission(root,f)
            self.assertFalse((root/'harbor-t044-consumed').exists()); self.assertFalse(list(f[2].iterdir()))
