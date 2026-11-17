import json
from unittest import TestCase
from clean_work_release_schema import FIELDS,parse


class CleanWorkReleaseSchema(TestCase):
    def document(self):
        doc={k:'x' for k in FIELDS}; doc.update({'schema':'planar-clean-work-account-v1','decision':'release-account','execution_id':'planar-t044-'+'1'*32,'terminal_class':'incomplete','exception':None,'reviewed_at':'2026-01-01T00:00:00+00:00','gate_path':'/test/gate','authority_path':'/test/authority','raw_manifest_path':'/test/raw','independent_review':True,'missing_artifacts':['final host capture'],'steps':{'worker':{'status':'pass','reason':None,'reward':1,'exception':None,'status_sha256':'b'*64,'reward_sha256':'c'*64},'review':{'status':'incomplete','reason':'audit pending','reward':0,'exception':None,'status_sha256':'d'*64,'reward_sha256':'e'*64}}})
        for k in FIELDS:
            if k.endswith('_sha256'): doc[k]='a'*64
        for k in ('historical_commit','historical_tree','resolver_commit'): doc[k]='a'*40
        return doc
    def test_one_step_requires_separate_raw_audit(self):
        doc=self.document(); doc['steps']['review']=None; doc['missing_artifacts']=['review verifier output','review native logs','post host capture','final host capture']
        with self.assertRaisesRegex(ValueError,'raw audit missing'): parse(json.dumps(doc))
        doc['raw_audit_path']='/test/audit'; doc['raw_audit_sha256']='a'*64
        self.assertIsNone(parse(json.dumps(doc))['steps']['review'])
        doc['raw_audit_path']='/test/../outside'
        with self.assertRaises(ValueError): parse(json.dumps(doc))

    def test_closed_typed_and_duplicates(self):
        doc=self.document(); self.assertEqual(parse(json.dumps(doc))['steps']['review']['status'],'incomplete')
        doc['steps']['review']['reward']=False
        with self.assertRaisesRegex(ValueError,'reward'): parse(json.dumps(doc))
        doc=self.document(); doc['independent_review']=1
        with self.assertRaisesRegex(ValueError,'independent'): parse(json.dumps(doc))
        with self.assertRaises(ValueError): parse('{"schema":"x","schema":"y"}')
        doc=self.document(); doc['unexpected']=1
        with self.assertRaisesRegex(ValueError,'schema'): parse(json.dumps(doc))
