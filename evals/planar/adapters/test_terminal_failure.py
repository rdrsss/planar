import hashlib
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch
import launch
import terminal_failure as tf
import terminal_failure_schema as schema

REVIEWS=launch.project_dir()/'reviews'
PREP=REVIEWS/'t045-retained-b-confirmed-prep-20261117'
EID='planar-t044-db1d390743d04c34b60e50123280e1a7'
EVIDENCE=PREP/'output/jobs'/EID
ROOT_AUDIT=REVIEWS/'t045-retained-b-timeout-root-authority-evidence-20261117.json'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

class ActualTimeoutEvidence(TestCase):
    def setUp(self):
        if not ROOT_AUDIT.is_file(): self.skipTest('reviewed timeout evidence unavailable')
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name); self.env=patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(PREP/'output'),'PLANAR_EVALS_LAYOUT_KIND':'override-jobs'}); self.env.start(); self.addCleanup(self.env.stop)
        self.start=PREP/'output/planar-account-admission-v1/records'/(EID+'.start.json'); self.gate=json.loads((PREP/'output/approved-review.json').read_text()); self.native=json.loads((EVIDENCE/'native-trial-start.json').read_text()); job=EVIDENCE/EID; self.trial=job/self.native['trial_name']; logs=EVIDENCE/'retained-b/logs/revision'; paths=[logs/'codex.txt']+list((logs/'sessions').rglob('rollout-*.jsonl')); self.manifest=self.root/'raw.json'; self.manifest.write_text(json.dumps({'artifacts':[{'path':str(p),'sha256':sha(p)} for p in paths]}))
        # The synthetic manifest lives in owned scratch. Substitute only its
        # bytes; all historical evidence keeps the real review-root checks.
        original_read=tf.read_safe
        def fixture_read(path, root, *args, **kwargs):
            if Path(path) == self.manifest: return self.manifest.read_bytes()
            return original_read(path,root,*args,**kwargs)
        self.read_patch=patch.object(tf,'read_safe',side_effect=fixture_read)
        self.read_patch.start(); self.addCleanup(self.read_patch.stop)
        self.original_read=original_read
        parent=REVIEWS/'t045-b-timeout-parent-audit-20261117.md'; child=REVIEWS/'t045-b-timeout-child-audit-20261117.md'; self.doc={'schema':'planar-terminal-failure-v1','type':'terminal-failure-v1','decision':'release-account','execution_id':EID,'job_id':self.native['job_id'],'trial_id':self.native['trial_id'],'gate_sha256':sha(PREP/'output/approved-review.json'),'source_packet_sha256':self.gate['source_packet_sha256'],'case_sha256':self.gate['case_sha256'],'candidate_sha256':self.gate['candidate_sha256'],'configuration_sha256':self.gate['configuration_sha256'],'effective_configuration_sha256':self.gate['effective_configuration_sha256'],'historical_commit':'adc3397e3f2cdb8346c57481b11151f8cd0779d1','historical_tree':'3cb14c25c9be95fe82855bb36771f437582404e7','historical_implementation_sha256':self.gate['implementation_sha256'],'resolver_commit':'a'*40,'resolver_implementation_sha256':'b'*64,'authority_path':str(ROOT_AUDIT),'authority_sha256':sha(ROOT_AUDIT),'parent_audit_path':str(parent),'parent_audit_sha256':sha(parent),'child_audit_path':str(child),'child_audit_sha256':sha(child),'raw_manifest_path':str(self.manifest),'raw_manifest_sha256':sha(self.manifest),'exception_sha256':sha(self.trial/'exception.txt'),'status_sha256':sha(self.trial/'verifier/status.json'),'reward_sha256':sha(self.trial/'verifier/reward.txt'),'reviewer':'independent','independence_basis':'separate review','reviewed_at':'2026-11-17T09:00:00+00:00','rationale':'partial work, missing final snapshot, attempt 2 unresolved','partial_work_reviewed':True,'missing_final_acknowledged':True,'independent_review':True,'attempt2_unresolved':True,'claim':'partial work occurred; final snapshot missing; attempt 2 outcome unresolved'}
    def test_actual_evidence_and_historical_inventory(self):
        doc=schema.parse(json.dumps(self.doc)); result=tf.verify_evidence(doc,EVIDENCE,self.start); self.assertEqual(result['trial_name'],self.native['trial_name'])
    def test_scratch_manifest_is_not_a_review_authority(self):
        with self.assertRaisesRegex(ValueError,'outside root'):
            self.original_read(self.manifest,REVIEWS)
    def test_mutated_bindings_reject(self):
        for key in ('job_id','trial_id','gate_sha256','historical_tree','status_sha256','exception_sha256'):
            with self.subTest(key=key),self.assertRaises((ValueError,OSError)): tf.verify_evidence({**self.doc,key:'wrong'},EVIDENCE,self.start)
    def test_strict_schema(self):
        for key in schema.BOOLS:
            for value in (False,1,'true'):
                with self.subTest(key=key,value=value),self.assertRaises(ValueError): schema.parse(json.dumps({**self.doc,key:value}))
        with self.assertRaises(ValueError): schema.parse(json.dumps(self.doc)[:-1]+',"decision":"release-account"}')

class SyntheticPublication(TestCase):
    def setUp(self):
        from types import SimpleNamespace
        import codex_run
        self.codex_run=codex_run; self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name).resolve(); self.project=self.root/'project'; self.reviews=self.project/'reviews'; self.reviews.mkdir(parents=True); self.scope=self.reviews/'output'; self.scope.mkdir(); self.eid='planar-t044-'+'1'*32; self.evidence=self.scope/'jobs'/self.eid; self.evidence.mkdir(parents=True)
        self.env=patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(self.scope)}); self.env.start(); self.addCleanup(self.env.stop); self.pj=patch.object(launch,'project_dir',return_value=self.project); self.pj.start(); self.addCleanup(self.pj.stop)
        self.records=self.scope/'planar-account-admission-v1/records'; self.records.mkdir(parents=True); self.start=self.records/(self.eid+'.start.json')
        self.doc={k:'a'*64 for k in schema.FIELDS-schema.BOOLS}; self.doc.update(schema='planar-terminal-failure-v1',type='terminal-failure-v1',decision='release-account',execution_id=self.eid,job_id='job',trial_id='trial',historical_commit='a'*40,historical_tree='b'*40,resolver_commit='c'*40,reviewer='reviewer',independence_basis='independent review',reviewed_at='2026-11-17T08:00:00+00:00',rationale='partial work, missing final, unresolved attempt 2',partial_work_reviewed=True,missing_final_acknowledged=True,independent_review=True,attempt2_unresolved=True,claim='partial work occurred; final snapshot missing; attempt 2 outcome unresolved')
        self.gate={k:self.doc[k] for k in ('source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256')}; self.gate['implementation_sha256']=self.doc['historical_implementation_sha256']; self.start.write_text(json.dumps({'execution_id':self.eid,'case':'retained-a-fresh-b','gate_sha256':self.doc['gate_sha256'],'gate_authority':self.gate})); self.review=self.reviews/'approved.json'; self.review.write_text(json.dumps(self.doc))
        self.patches=[patch.object(tf,'verify_evidence',return_value={'trial_name':'trial'}),patch.object(tf,'live_check',return_value={'execution_id':self.eid,'native_group':'absent','project_count':2,'matching_commands':0,'unresolved':0}),patch('retained_b_early_release.resolver_identity',return_value={'commit':'c'*40,'implementation_sha256':'a'*64}),patch.object(tf.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=''))]
        for p in self.patches: p.start(); self.addCleanup(p.stop)
    def call(self): return tf.publish(self.evidence,self.review)
    def test_atomic_publication_requires_lock_and_refresh(self):
        self.assertEqual(self.call()['behavior'],'incomplete')
        self.assertEqual(len(self.codex_run.shared_pending(self.records)),1)
        with self.codex_run.admission_scope() as records:
            self.assertEqual(self.codex_run.shared_pending(records),[])
            with patch.object(tf,'live_check',side_effect=ValueError('resource reappeared')): self.assertEqual(len(self.codex_run.shared_pending(records)),1)
        self.assertEqual(self.call()['behavior'],'incomplete')
    def test_fault_conflict_and_wrong_types_stay_pending(self):
        with patch.object(tf.os,'link',side_effect=OSError('interrupted')):
            with self.assertRaises(OSError): self.call()
        self.assertEqual(len(self.codex_run.shared_pending(self.records)),1)
        marker=self.records/(self.eid+'.resolved.json'); marker.write_text('{')
        with self.assertRaises(ValueError): self.call()
        self.assertEqual(marker.read_text(),'{'); self.assertEqual(len(self.codex_run.shared_pending(self.records)),1)
    def test_consumer_rejects_mutation_and_boolean_reward(self):
        self.call(); marker=self.records/(self.eid+'.resolved.json'); data=json.loads(marker.read_text()); original=self.review.read_bytes()
        for key,value in (('resolver_commit','d'*40),('resolver_implementation_sha256','e'*64),('observed_reward',False),('job_id','wrong')):
            marker.write_text(json.dumps({**data,key:value}))
            with self.subTest(key=key),self.codex_run.admission_scope() as records: self.assertEqual(len(self.codex_run.shared_pending(records)),1)
            self.assertEqual(self.review.read_bytes(),original)
    def test_census_and_review_tamper_stay_pending(self):
        self.call(); marker=self.records/(self.eid+'.resolved.json'); data=json.loads(marker.read_text()); census=Path(data['census_path']); old=census.read_bytes(); census.write_bytes(old+b' ')
        with self.codex_run.admission_scope() as records: self.assertEqual(len(self.codex_run.shared_pending(records)),1)
        census.write_bytes(old); reviewed=self.review.read_bytes()
        altered=json.dumps({**self.doc,'independent_review':1}).encode(); self.review.write_bytes(altered); marker.write_text(json.dumps({**data,'review_sha256':tf.h(altered)}))
        with self.codex_run.admission_scope() as records: self.assertEqual(len(self.codex_run.shared_pending(records)),1)
        self.review.write_bytes(reviewed)
    def test_lock_contention_prevents_publish(self):
        with self.codex_run.admission_scope():
            with self.assertRaises((BlockingIOError,OSError)): self.call()
        self.assertEqual(len(self.codex_run.shared_pending(self.records)),1)

class ResourceChecks(TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.evidence=Path(self.tmp.name)/('planar-t044-'+'2'*32); self.evidence.mkdir(); self.eid=self.evidence.name; self.trial='retained-a-fresh-b__fixture'; (self.evidence/'harbor-process.json').write_text(json.dumps({'execution_id':self.eid,'pid':999999})); (self.evidence/'planar-cleanup.json').write_text(json.dumps({'execution_id':self.eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'})); registry=self.evidence/'projects'; registry.mkdir()
        for i,suffix in enumerate(('__env','__verifier__trial')): (registry/(str(i)+'.json')).write_text(json.dumps({'execution_id':self.eid,'compose_project':(self.eid+'-'+self.trial).lower()+suffix}))
    def test_cleanup_positive_and_unknown_resource_negative(self):
        from types import SimpleNamespace
        good=SimpleNamespace(returncode=0,stdout=''); bad=SimpleNamespace(returncode=0,stdout='present')
        with patch.object(tf.os,'killpg',side_effect=ProcessLookupError),patch.object(tf.subprocess,'run',return_value=good),patch.object(tf.retained_b_census,'check',return_value={'matching_commands':0,'unresolved':0}):
            self.assertEqual(tf.live_check(self.evidence,self.trial)['native_group'],'absent')
            with patch.object(tf.subprocess,'run',return_value=bad),self.assertRaises(ValueError): tf.live_check(self.evidence,self.trial)
            with patch.object(tf.retained_b_census,'check',side_effect=ValueError('unknown')),self.assertRaises(ValueError): tf.live_check(self.evidence,self.trial)
            (self.evidence/'recovery-required').write_text('pending')
            with self.assertRaises(ValueError): tf.live_check(self.evidence,self.trial)
    def test_live_or_unknown_process_fails_closed(self):
        with patch.object(tf.os,'killpg',return_value=None),self.assertRaises(ValueError): tf.live_check(self.evidence,self.trial)
        with patch.object(tf.os,'killpg',side_effect=PermissionError),self.assertRaises(ValueError): tf.live_check(self.evidence,self.trial)
