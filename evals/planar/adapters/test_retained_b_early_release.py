import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch
import codex_run
import launch
import retained_b_early_release as release
import retained_b_early_evidence as ev

CLEANUP_CHECK=release.cleanup_check
REAL_WRAPPER=launch.project_dir()/'reviews'/release.WRAPPER_AUDIT

class EarlyRelease(TestCase):
    def setUp(self):
        if not REAL_WRAPPER.is_file(): self.skipTest('reviewed wrapper evidence unavailable')
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name).resolve(); self.project=self.root/'project'; self.reviews=self.project/'reviews'; self.reviews.mkdir(parents=True); self.scope=self.reviews/'output'; self.scope.mkdir(); self.job=self.scope/'jobs'/ev.EID; self.job.mkdir(parents=True); self.records=self.scope/'planar-account-admission-v1/records'; self.records.mkdir(parents=True)
        self.env=patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(self.scope)}); self.env.start(); self.addCleanup(self.env.stop)
        self.project_patch=patch.object(launch,'project_dir',return_value=self.project); self.project_patch.start(); self.addCleanup(self.project_patch.stop)
        self.gate={'case_sha256':ev.CASE_SHA,'candidate_sha256':ev.CANDIDATE_SHA,'source_packet_sha256':ev.SOURCE_SHA,'configuration_sha256':'c'*64,'implementation_sha256':'d'*64}; (self.scope/'approved-review.json').write_text(json.dumps(self.gate))
        (self.records/(ev.EID+'.start.json')).write_text(json.dumps({'case':'retained-a-fresh-b','execution_id':ev.EID,'gate_sha256':ev.GATE_SHA,'gate_authority':self.gate}))
        self.wrapper=self.reviews/release.WRAPPER_AUDIT; self.wrapper.write_bytes(REAL_WRAPPER.read_bytes())
        self.review=self.reviews/'release.json'; self.identity={'commit':'a'*40,'implementation_sha256':'b'*64}
        self.observed={'historical_implementation':{'implementation_sha256':'d'*64,'commit':'bd3fecfe1c4786125973fc800150173f52be1f53'},'trace':{'trace_sha256':'7ebac5fa973365f637365c54fdb675852623304767f45c6c9c895049a4b1e0cc'}}
        self.doc={'trace_complete':True,'unlogged_work_assessed':True,'independent_review':True,'type':'aborted-before-work','decision':'release-account','reviewer':'independent reviewer','independence_basis':'separate review','reviewed_at':'2026-11-17T06:50:00+00:00','claim':'complete trace and state corroborate no B check, review or repair; unlogged work assessed','rationale':'reviewed evidence','execution_id':ev.EID,'job_id':ev.JOB,'trial_id':ev.TRIAL,'gate_sha256':ev.GATE_SHA,'source_packet_sha256':ev.SOURCE_SHA,'case_sha256':ev.CASE_SHA,'candidate_sha256':ev.CANDIDATE_SHA,'configuration_sha256':'c'*64,'historical_implementation_sha256':'d'*64,'resolver_commit':'a'*40,'resolver_implementation_sha256':'b'*64,'audit_sha256':ev.AUDIT_SHA,'wrapper_audit_sha256':release.digest(self.wrapper.read_bytes()),'trace_sha256':self.observed['trace']['trace_sha256'],'verifier_status_sha256':'d638a8043ee92815310aecbeb69060bc3a5464e08ff76f12b40252cfe0ae946e','reward_sha256':'9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa'}; self.review.write_text(json.dumps(self.doc))
        for target,value in (('ev.inspect',self.observed),('resolver_identity',self.identity),('cleanup_check',{}),('retained_b_census.check',{'matching_commands':0,'unresolved':0})):
            p=patch('retained_b_early_release.'+target,return_value=value); p.start(); self.addCleanup(p.stop)
    def call(self): return release.resolve(self.job,self.root,self.root,self.root,self.root,self.review)
    def test_valid_marker_and_idempotence(self):
        self.assertEqual(self.call()['behavior'],'aborted-before-work'); self.assertEqual(codex_run.shared_pending(self.records),[])
        self.assertEqual(self.call()['behavior'],'aborted-before-work')
        original=self.review.read_bytes(); self.review.write_bytes(original+b' ')
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        self.review.write_bytes(original); self.assertEqual(codex_run.shared_pending(self.records),[])
        census=next(self.records.glob('*.census-*.json')); original=census.read_bytes(); census.write_bytes(original+b' ')
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
    def test_shared_consumer_rejects_each_review_binding_mutation(self):
        self.call(); marker=self.records/(ev.EID+'.resolved.json'); original=json.loads(marker.read_text()); original_review=self.review.read_bytes()
        overlapping=set(original)&set(self.doc)
        self.assertIn('resolver_commit',overlapping); self.assertIn('resolver_implementation_sha256',overlapping)
        for key in sorted(overlapping):
            old=original[key]; replacement=('a' if old[0]!='a' else 'b')+old[1:]
            marker.write_text(json.dumps({**original,key:replacement}))
            with self.subTest(key=key):
                self.assertEqual(self.review.read_bytes(),original_review)
                self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        for value in (False,True,0.0,'0',None):
            marker.write_text(json.dumps({**original,'observed_reward':value}))
            with self.subTest(reward=value): self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        marker.write_text(json.dumps(original)); self.assertEqual(codex_run.shared_pending(self.records),[])
        marker.write_text(json.dumps(original)[:-1]+', \"observed_reward\": 0}')
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
    def test_shared_consumer_requires_strict_review_provenance_booleans(self):
        self.call(); marker=self.records/(ev.EID+'.resolved.json'); original=json.loads(marker.read_text()); raw=self.review.read_bytes()
        for key in ('trace_complete','unlogged_work_assessed','independent_review'):
            for value in (False,1,'true',None):
                changed=json.dumps({**self.doc,key:value}).encode(); self.review.write_bytes(changed)
                marker.write_text(json.dumps({**original,'release_sha256':release.digest(changed)}))
                with self.subTest(key=key,value=value): self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        self.review.write_bytes(raw); marker.write_text(json.dumps(original)); self.assertEqual(codex_run.shared_pending(self.records),[])
    def test_fault_and_conflict_remain_pending(self):
        with patch.object(release.os,'link',side_effect=OSError('prepublication')):
            with self.assertRaises(OSError): self.call()
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        marker=self.records/(ev.EID+'.resolved.json'); marker.write_text('{')
        with self.assertRaises((ValueError,json.JSONDecodeError)): self.call()
        self.assertEqual(marker.read_text(),'{'); self.assertEqual(len(codex_run.shared_pending(self.records)),1)
    def test_incomplete_or_uncertain_evidence_remains_pending(self):
        for target in ('ev.inspect','cleanup_check','retained_b_census.check','resolver_identity'):
            with self.subTest(target=target), patch('retained_b_early_release.'+target,side_effect=ValueError('unknown')):
                with self.assertRaises(ValueError): self.call()
                self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        for field in ('gate_sha256','trace_sha256','resolver_commit','wrapper_audit_sha256','claim'):
            self.review.write_text(json.dumps({**self.doc,field:'wrong'}))
            with self.subTest(field=field),self.assertRaises(ValueError): self.call()
            self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        self.review.write_text(json.dumps(self.doc))
    def test_actual_cleanup_check_fails_closed(self):
        for name in ('launcher-terminal.json','watchdog-terminal.json'):
            (self.job/name).write_text(json.dumps({'execution_id':ev.EID,'status':'completed','return_code':0}))
        (self.job/'planar-cleanup.json').write_text(json.dumps({'execution_id':ev.EID,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'}))
        (self.job/'harbor-process.json').write_text(json.dumps({'execution_id':ev.EID,'pid':88111}))
        registry=self.job/'projects'; registry.mkdir()
        for i,suffix in enumerate(('env','verifier__trial')):
            (registry/(str(i)+'.json')).write_text(json.dumps({'execution_id':ev.EID,'compose_project':ev.EID+'-retained-a-fresh-b__qsdfojr__'+suffix}))
        with patch.object(release.os,'killpg',side_effect=ProcessLookupError),patch.object(release.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout='')):
            self.assertEqual(CLEANUP_CHECK(self.job)['exact_projects'],2)
            with patch.object(release.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout='present')):
                with self.assertRaises(ValueError): CLEANUP_CHECK(self.job)
            (self.job/'recovery-required').write_text('pending')
            with self.assertRaises(ValueError): CLEANUP_CHECK(self.job)
    def test_wrong_binding_and_lock_fail(self):
        self.review.write_text(json.dumps({**self.doc,'source_packet_sha256':'wrong'}))
        with self.assertRaises(ValueError): self.call()
        self.review.write_text(json.dumps(self.doc))
        with codex_run.admission_scope():
            with self.assertRaises((OSError,BlockingIOError)): self.call()
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
