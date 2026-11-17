from datetime import datetime,timezone,timedelta
from pathlib import Path
import unittest
from retained_b_support import validate_gate

class RetainedGateControls(unittest.TestCase):
    def setUp(self):
        self.now=datetime(2026,1,1,tzinfo=timezone.utc); self.expiry=(self.now+timedelta(seconds=1100)).isoformat(); self.eid='planar-t044-'+'1'*32; self.auth=Path('/not-read/auth.json'); self.scope=Path('/scope'); self.verifier='sha256:'+'v'*64
        self.snapshot={'candidate_sha256':'candidate','case_sha256':'casehash','implementation_sha256':'impl','configuration_sha256':'config','image_id':'image','prerequisites':{'effective_configuration_sha256':'effective','source_packet_sha256':'source','source_packet_root':'/source'}}
        self.policy={'execution':'source-id','assessor_image':'source-image','files':{'evidence/corrected-assessment.txt':{'sha256':'assessment'},'evidence/semantic-audit.md':{'sha256':'audit'}}}
        self.gate={'verdict':'approve','purpose':'one-diagnostic-attempt','case':'retained-a-fresh-b','max_attempts':1,'execution_id':self.eid,'absolute_deadline':self.expiry,'candidate_sha256':'candidate','case_sha256':'casehash','implementation_sha256':'impl','configuration_sha256':'config','effective_configuration_sha256':'effective','source_packet_sha256':'source','source_packet_root':'/source','source_execution_id':'source-id','source_online_status':'behavioral_failure','source_online_reward':0,'source_assessor_image':'source-image','source_assessment_sha256':'assessment','source_semantic_audit_sha256':'audit','image_id':'image','verifier_image_id':self.verifier,'account_scope_id':str(self.scope),'runtime_budget_seconds':1050,'step_agent_seconds':400,'step_verifier_seconds':60,'cleanup_grace_seconds':15,'result_boundary':'incomplete-pending-independent-bound-raw-audit','auth_path':str(self.auth)}
    def run_gate(self): return validate_gate(self.gate,self.snapshot,self.policy,self.eid,self.expiry,self.auth,self.scope,self.verifier,now=self.now)
    def test_exact_gate_without_auth_access(self): self.assertEqual(self.run_gate()['execution_id'],self.eid)
    def test_identity_budget_and_result_drift_rejected(self):
        for key in ('execution_id','source_packet_sha256','effective_configuration_sha256','account_scope_id','verifier_image_id','result_boundary','source_online_reward'):
            old=self.gate[key]; self.gate[key]=False if key=='source_online_reward' else 'wrong'
            with self.subTest(key=key),self.assertRaises(ValueError): self.run_gate()
            self.gate[key]=old
    def test_expiry_and_auth_path_rejected(self):
        self.now+=timedelta(seconds=51)
        with self.assertRaisesRegex(ValueError,'insufficient'): self.run_gate()
        self.now-=timedelta(seconds=51); self.gate['auth_path']='/other'
        with self.assertRaisesRegex(ValueError,'auth path'): self.run_gate()

if __name__=='__main__': unittest.main()
