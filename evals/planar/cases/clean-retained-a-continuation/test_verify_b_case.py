import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from datetime import datetime, timezone
from links import TASK
from verify_b_fixture import evidence
from verify_b import assess


class TargetVerifier(unittest.TestCase):
    def evaluate(self, edit=None):
        pre,post,dispatch,final,parents,children=evidence()
        for snapshot in (pre,post,final): snapshot['host/git-stage.txt']=b'base stage\n'
        if edit: edit(pre,post,final,parents,children)
        stamp=int(datetime(2026,1,1,0,0,15,750000,tzinfo=timezone.utc).timestamp()*1e9)
        receipts={'pre':{'wall_ns':stamp-4,'monotonic_ns':1},'restored-pre':{'wall_ns':stamp-3,'monotonic_ns':2},
                  'post':{'wall_ns':stamp-2,'monotonic_ns':3},'final':{'wall_ns':stamp+2_000_000_000,'monotonic_ns':4}}
        with TemporaryDirectory() as tmp:
            execution=Path(tmp)/('planar-t044-'+'a'*32); execution.mkdir(); root=execution/'retained-clean'; root.mkdir(); packet=Path(tmp)/'packet'; (packet/'source').mkdir(parents=True)
            (root/'no-fault.json').write_text(json.dumps({'source_packet_sha256':'digest','source_job':'13e093d0-85e3-4001-ba76-44d9d108eb6b','target_job':'target-job','boundary':'restored-no-fault','wall_ns':stamp-1,'monotonic_ns':3}))
            (root/'target-provenance.json').write_text(json.dumps({'execution_id':'planar-t044-'+'a'*32,'job_id':'target-job'}))
            def load(path): return {'pre':pre,'restored-pre':pre,'post':post,'final':final}[path.name]
            with patch('verify_b.validate_closed',return_value={'source_packet_sha256':'digest'}), \
                 patch('verify_b.receipt',side_effect=lambda _,name:receipts[name]), patch('verify_b.load',side_effect=load), \
                 patch('verify_b.load_target',return_value=(parents[1],[children[1]])), \
                 patch('verify_b.correlate_target',return_value={'parents':['new'],'children':['reviewer']}), \
                 patch('delivery.verify_child_read',return_value={'call_id':'child-read'}):
                return assess(packet,'candidate',root)

    def test_clean_review_remains_pending_semantic_audit(self):
        result=self.evaluate(); self.assertEqual(result['status'],'incomplete'); self.assertEqual(result.get('structural_checks'),'passed')

    def test_missing_final_check_is_incomplete(self):
        def edit(a,b,f,p,c): p[1][:]=[x for x in p[1] if x.get('payload',{}).get('call_id')!='final-check']
        self.assertNotEqual(self.evaluate(edit).get('structural_checks'),'passed')

    def test_unauthorized_home_change_fails(self):
        def edit(a,b,f,p,c): f['home/projects/revision-fault/prd.md']=b'changed'
        self.assertEqual(self.evaluate(edit)['status'],'behavioral_failure')

    def test_reviewer_completion_reuse_is_allowed_but_foreign_reuse_fails(self):
        def valid(a,b,f,p,c):
            f[TASK+'record.md']=f[TASK+'record.md'].replace(b'review-1-all.md: approve',b'review-1-all.md: approve; evidence session-R-1-3.md')
        self.assertEqual(self.evaluate(valid).get('structural_checks'),'passed')
        def invalid(a,b,f,p,c):
            f[TASK+'record.md']=f[TASK+'record.md'].replace(b'review-1-all.md: approve',b'review-1-all.md: approve; evidence session-R-1-2.md')
        self.assertNotEqual(self.evaluate(invalid).get('structural_checks'),'passed')

    def test_wrong_reviewer_role_is_incomplete(self):
        def edit(a,b,f,p,c): f[TASK+'session-R-1-1.md']=f[TASK+'session-R-1-1.md'].replace(b'role: reviewer',b'role: worker')
        self.assertNotEqual(self.evaluate(edit).get('structural_checks'),'passed')

    def test_failed_final_check_is_rejected(self):
        def edit(a,b,f,p,c):
            out=next(x for x in p[1] if x.get('payload',{}).get('call_id')=='final-check' and x['payload']['type']=='custom_tool_call_output')
            out['payload']['output'][1]['text']=json.dumps({'exit_code':1,'output':'','wall_time_seconds':0.1})
        self.assertEqual(self.evaluate(edit)['status'],'behavioral_failure')

    def test_nonapproval_is_preserved_without_fabricated_pass(self):
        def edit(a,b,final,p,c): final[TASK+'review-1-all.md']=b'# Review\n\nverdict: request-changes\nstatus: complete\n'
        result=self.evaluate(edit); self.assertEqual(result['status'],'incomplete'); self.assertEqual(result.get('reported_review_verdict'),'request-changes')

    def test_changed_product_does_not_pass(self):
        def edit(a,b,final,p,c): b['repository/STATE.txt']=b'fault-injected\n'
        self.assertNotEqual(self.evaluate(edit).get('structural_checks'),'passed')
