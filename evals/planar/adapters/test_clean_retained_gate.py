from datetime import datetime,timezone,timedelta
import copy
import unittest
from clean_retained_gate import validate


class Gate(unittest.TestCase):
    def test_exact_binding_and_negative_controls(self):
        now=datetime(2026,1,1,tzinfo=timezone.utc); expiry=(now+timedelta(seconds=1050)).isoformat(); eid='planar-t044-'+'a'*32
        snapshot={'candidate_sha256':'candidate','case_sha256':'case','implementation_sha256':'impl','configuration_sha256':'config','image_id':'agent',
                  'prerequisites':{'effective_configuration_sha256':'effective','source_packet_sha256':'packet','source_packet_root':'/packet','source_account_scope_id':'/source-scope'}}
        policy={'execution':'source','assessor_image':'assessor','worker_role_delivery':'unverified','source_semantics':'packet-read-causal-write-completion-only','files':{'evidence/semantic-audit.md':{'sha256':'audit'},'evidence/offline-regrade.json':{'sha256':'regrade'}}}
        gate={'verdict':'approve','purpose':'one-diagnostic-attempt','case':'clean-retained-a-continuation','max_attempts':1,'execution_id':eid,'absolute_deadline':expiry,
              'candidate_sha256':'candidate','case_sha256':'case','implementation_sha256':'impl','configuration_sha256':'config','effective_configuration_sha256':'effective',
              'source_packet_sha256':'packet','source_packet_root':'/packet','source_account_scope_id':'/source-scope','source_execution_id':'source','source_online_status':'behavioral_failure','source_online_reward':0,
              'source_assessor_image':'assessor','source_assessment_sha256':'regrade','source_semantic_audit_sha256':'audit','worker_role_delivery':'unverified','source_semantics':'packet-read-causal-write-completion-only',
              'handoff':'no-fault','model_name':'gpt-6.1-terra','reasoning_effort':'medium','image_id':'agent','verifier_image_id':'verifier','account_scope_id':'/scope','runtime_budget_seconds':1050,'step_agent_seconds':400,
              'step_verifier_seconds':60,'cleanup_grace_seconds':15,'result_boundary':'incomplete-pending-independent-bound-raw-audit','auth_path':'/auth'}
        def invoke(value, at=now): return validate(value,snapshot,policy,eid,expiry,'/auth','/scope','verifier',now=at)
        self.assertEqual(invoke(gate)['execution_id'],eid)
        for key,value in (('source_online_reward',False),('worker_role_delivery','verified'),('handoff','injected'),('source_packet_sha256','wrong'),('verifier_image_id','wrong'),('max_attempts',2)):
            changed=copy.deepcopy(gate);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): invoke(changed)
        with self.assertRaises(ValueError): invoke(gate,now+timedelta(seconds=1))

    def test_unknown_resolution_marker_never_releases_new_case(self):
        import json
        from pathlib import Path
        from tempfile import TemporaryDirectory
        import codex_run
        with TemporaryDirectory() as tmp:
            records=Path(tmp); start=records/'x.start.json'; start.write_text(json.dumps({'case':'clean-retained-a-continuation'}))
            (records/'x.resolved.json').write_text('{}')
            self.assertEqual(codex_run.shared_pending(records),[start])
            (records/'x.closed.json').write_text('{}')
            self.assertEqual(codex_run.shared_pending(records),[start])
