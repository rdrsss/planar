import copy
import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import clean_retained_release as release

FIXTURE=Path(__file__).parent/'fixtures/retained-clean-disposition/native-v5-redacted.json'


class ProducerShape(unittest.TestCase):
    def setUp(self):
        f=json.loads(FIXTURE.read_bytes()); self.assertTrue(f['synthetic_noauth_only']); self.data=f['data']
        self.job=self.data['job']; self.trial=self.data['trial']; self.config=self.data['config']
    def check(self): release.validate_shape(self.job,self.trial,self.config,0,self.job['id'],self.trial['id'],self.trial['trial_name'],Path('/fixture/job'))
    def test_redacted_producer_shape(self): self.check()
    def test_unstepped_reward_exception_and_timing_negatives(self):
        for key,value in (('step_results',[]),('reward_valid',False),('exception_info',{}),('verifier_environment_mode','inline'),('verifier_result',{'rewards':{'reward':1}})):
            with self.subTest(key=key):
                old=self.trial[key]; self.trial[key]=value
                with self.assertRaises((ValueError,TypeError)): self.check()
                self.trial[key]=old
        self.trial['verifier']['finished_at']=None
        with self.assertRaises((ValueError,TypeError)): self.check()
    def test_job_or_trial_identity_contradiction(self):
        self.job['stats']['n_running_trials']=1
        with self.assertRaises(ValueError): self.check()
        self.job['stats']['n_running_trials']=0; self.trial['config']['job_id']='00000000-0000-0000-0000-000000000000'
        with self.assertRaises(ValueError): self.check()


class Cleanup(unittest.TestCase):
    def test_both_labels_queried_even_when_registry_missing(self):
        with TemporaryDirectory() as tmp:
            eid='planar-t044-'+'1'*32; evidence=Path(tmp)/eid; evidence.mkdir(); seen=[]
            def run(args,**kw): seen.append(args[-1]); return SimpleNamespace(returncode=0,stdout='')
            with patch('clean_retained_release.read_safe',side_effect=FileNotFoundError):
                with self.assertRaises(FileNotFoundError): release.live_check(evidence,'trial',{'step_results':None},run=run)
            self.assertEqual(len(seen),6); self.assertEqual(len(set(seen)),2)
    def test_failed_resource_query_rejects(self):
        with TemporaryDirectory() as tmp:
            evidence=Path(tmp)/('planar-t044-'+'1'*32); evidence.mkdir()
            with self.assertRaises(ValueError): release.live_check(evidence,'trial',{},run=lambda *a,**k:SimpleNamespace(returncode=1,stdout=''))


class Schema(unittest.TestCase):
    def test_rejects_nonreview_and_boolean_zero(self):
        doc={k:'a'*64 for k in release.FIELDS};doc.update({'schema':'planar-retained-clean-account-v1','decision':'release-account','execution_id':'planar-t044-'+'1'*32,'job_id':'target','trial_id':'trial','source_job_id':'source','source_status':'behavioral_failure','source_reward':0,'target_status':'incomplete','target_reward':0,'target_reason':None,'target_reason_present':False,'independent_review':True,'reviewed_at':'2026-01-01T00:00:00+00:00','gate_path':'/gate','authority_path':'/authority','raw_manifest_path':'/manifest','raw_audit_path':'/audit','historical_commit':'a'*40,'historical_tree':'b'*40,'resolver_commit':'c'*40})
        self.assertEqual(release.parse(json.dumps(doc))['target_status'],'incomplete')
        for name,value in (('source_reward',False),('target_reward',False),('independent_review',1),('target_reason_present',0)):
            altered=copy.deepcopy(doc); altered[name]=value
            with self.subTest(name=name),self.assertRaises(ValueError): release.parse(json.dumps(altered))


def document(root):
    doc={k:'a'*64 for k in release.FIELDS}
    doc.update({'schema':'planar-retained-clean-account-v1','decision':'release-account','execution_id':'planar-t044-'+'1'*32,
        'job_id':'target','trial_id':'trial','source_job_id':'source','source_status':'behavioral_failure','source_reward':0,
        'target_status':'incomplete','target_reward':0,'target_reason':None,'target_reason_present':False,'independent_review':True,
        'reviewed_at':'2026-01-01T00:00:00+00:00','gate_path':str(root/'gate'),'authority_path':str(root/'authority'),
        'raw_manifest_path':str(root/'manifest'),'raw_audit_path':str(root/'audit'),'historical_commit':'a'*40,'historical_tree':'b'*40,'resolver_commit':'c'*40})
    return doc


class Publication(unittest.TestCase):
    def test_locked_publication_fault_idempotence_and_consumer_tamper(self):
        import codex_run
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs';jobs.mkdir(); doc=document(root); eid=doc['execution_id']; evidence=jobs/eid;evidence.mkdir(); review=root/'review.json';review.write_text(json.dumps(doc))
            census={'execution_id':eid,'native_group':'absent','matching_commands':0,'unresolved':0}
            with patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs), \
                 patch.object(release,'verify',return_value={'trial_name':'trial','trial':{}}),patch.object(release,'live_check',return_value=census), \
                 patch.object(release,'resolver_identity',return_value={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}):
                with codex_run.admission_scope() as records: (records/(eid+'.start.json')).write_text(json.dumps({'case':'clean-retained-a-continuation','execution_id':eid}))
                with patch.object(release.os,'link',side_effect=OSError('injected')):
                    with self.assertRaises(OSError): release.publish(evidence,review)
                self.assertEqual(len(codex_run.shared_pending(records)),1)
                self.assertEqual(release.publish(evidence,review)['account'],'resolved')
                self.assertEqual(len(codex_run.shared_pending(records)),1)
                with codex_run.admission_scope() as locked: self.assertEqual(codex_run.shared_pending(locked),[])
                self.assertEqual(release.publish(evidence,review)['account'],'resolved')
                with patch.object(release,'live_check',side_effect=ValueError('active resource')):
                    with codex_run.admission_scope() as locked: self.assertEqual(len(codex_run.shared_pending(locked)),1)
                marker=records/(eid+'.resolved.json'); value=json.loads(marker.read_text()); value['review_binding']['target_reward']=False; marker.write_bytes(release.canonical(value)+b'\n')
                with codex_run.admission_scope() as locked: self.assertEqual(len(codex_run.shared_pending(locked)),1)
                with self.assertRaisesRegex(ValueError,'conflicting'): release.publish(evidence,review)

    def test_live_cleanup_exact_producer_and_both_labels(self):
        import retained_b_census
        with TemporaryDirectory() as tmp:
            project=Path(tmp).resolve(); reviews=project/'reviews';reviews.mkdir(); eid='planar-t044-'+'2'*32; evidence=reviews/eid;evidence.mkdir(); registry=evidence/'projects';registry.mkdir(); trial_name='task__Trial'; prefix=(eid+'-'+trial_name).lower(); names={prefix+'__env',prefix+'__verifier__trial'}
            def put(path,obj): path.write_text(json.dumps(obj))
            for name in names: put(registry/(hashlib.sha256(name.encode()).hexdigest()+'.json'),{'execution_id':eid,'compose_project':name})
            for name in ('launcher-terminal.json','watchdog-terminal.json'): put(evidence/name,{'execution_id':eid,'status':'completed','return_code':0})
            put(evidence/'harbor-process.json',{'execution_id':eid,'pid':99999}); put(evidence/'planar-terminal.json',{'execution_id':eid,'status':'shell_completed','return_code':0}); put(evidence/'planar-cleanup.json',{'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'})
            put(evidence/'verifier-compose-up.json',{'execution_id':eid,'command':['up','--detach','--wait','--no-build','--pull','never']}); gate=reviews/'gate.json';put(gate,{'verifier_image_id':'image'})
            put(evidence/'retained-admission.json',{'gate_path':str(gate)});put(evidence/'verifier-network.json',{'execution_id':eid,'container_id':'a'*12,'network_mode':'none','image_id':'image'})
            seen=[]
            def run(args,**kw): seen.append(args[-1]); return SimpleNamespace(returncode=0,stdout='')
            with patch.object(release.launch,'PROJECT',project),patch.object(release.os,'killpg',side_effect=ProcessLookupError),patch.object(retained_b_census,'check',return_value={'matching_commands':0,'unresolved':0}):
                result=release.live_check(evidence,trial_name,{'step_results':None},run=run); self.assertEqual(set(result['queried_projects']),names);self.assertEqual(len(seen),6)
                (evidence/'planar-terminal.json').write_text(json.dumps({'execution_id':eid,'status':'shell_completed','return_code':False}))
                with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=run)
                put(evidence/'planar-terminal.json',{'execution_id':eid,'status':'shell_completed','return_code':0}); (registry/(hashlib.sha256((prefix+'__env').encode()).hexdigest()+'.json')).unlink();seen.clear()
                with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=run)
                self.assertEqual(len(seen),6)


class StaticBindings(unittest.TestCase):
    def test_gate_start_source_and_target_mutations_reject(self):
        with TemporaryDirectory() as tmp:
            project=Path(tmp).resolve(); reviews=project/'reviews';reviews.mkdir(); jobs=reviews/'jobs';jobs.mkdir(); records=reviews/'planar-account-admission-v1/records';records.mkdir(parents=True); doc=document(reviews); eid=doc['execution_id']; evidence=jobs/eid;evidence.mkdir(); start=records/(eid+'.start.json'); gate_path=reviews/'gate'
            gate={'case':'clean-retained-a-continuation','execution_id':eid,'verdict':'approve','purpose':'one-diagnostic-attempt','max_attempts':1,
                  'case_sha256':doc['case_sha256'],'candidate_sha256':doc['candidate_sha256'],'implementation_sha256':doc['implementation_sha256'],
                  'configuration_sha256':doc['configuration_sha256'],'effective_configuration_sha256':doc['effective_configuration_sha256'],
                  'source_packet_sha256':doc['source_packet_sha256'],'worker_role_delivery':'unverified','source_semantics':'packet-read-causal-write-completion-only',
                  'source_online_status':'behavioral_failure','source_online_reward':0,'handoff':'no-fault','account_scope_id':str(records.parent)}
            raw=json.dumps(gate).encode();gate_path.write_bytes(raw);doc['gate_sha256']=release.digest(raw); start.write_text(json.dumps({'execution_id':eid,'case':'clean-retained-a-continuation','gate_sha256':doc['gate_sha256'],'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'}));doc['start_sha256']=release.digest(start.read_bytes())
            with patch.object(release.launch,'PROJECT',project),patch.object(release.launch,'reviews_root',return_value=reviews),patch.object(release.launch,'jobs_root',return_value=jobs):
                for key in ('case_sha256','source_packet_sha256','execution_id','gate_sha256','start_sha256'):
                    changed=copy.deepcopy(doc);changed[key]='bad'
                    with self.subTest(key=key),self.assertRaises(ValueError): release.verify(changed,evidence,start)

    def test_review_swap_before_publication_is_rejected(self):
        import codex_run
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs';jobs.mkdir(); doc=document(root);eid=doc['execution_id'];evidence=jobs/eid;evidence.mkdir();review=root/'review.json';review.write_text(json.dumps(doc))
            def swapped(*args):
                review.write_bytes(review.read_bytes()+b' ')
                return {'execution_id':eid,'native_group':'absent','matching_commands':0,'unresolved':0}
            with patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs),patch.object(release,'verify',return_value={'trial_name':'trial','trial':{}}),patch.object(release,'live_check',side_effect=swapped),patch.object(release,'resolver_identity',return_value={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}):
                with codex_run.admission_scope() as records: (records/(eid+'.start.json')).write_text(json.dumps({'case':'clean-retained-a-continuation','execution_id':eid}))
                with self.assertRaisesRegex(ValueError,'review bytes changed'): release.publish(evidence,review)
                self.assertFalse((records/(eid+'.resolved.json')).exists())
