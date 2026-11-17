import copy
import hashlib
import json
import os
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
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): selected_context=release.launch.context()
            with patch.object(release.launch,'context',return_value=selected_context),patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs), \
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
            with patch.object(release.launch,'project_dir',return_value=project),patch.object(release.os,'killpg',side_effect=ProcessLookupError),patch.object(retained_b_census,'check',return_value={'matching_commands':0,'unresolved':0}):
                result=release.live_check(evidence,trial_name,{'step_results':None},run=run); self.assertEqual(set(result['queried_projects']),names);self.assertEqual(len(seen),6)
                (evidence/'planar-terminal.json').write_text(json.dumps({'execution_id':eid,'status':'shell_completed','return_code':False}))
                with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=run)
                put(evidence/'planar-terminal.json',{'execution_id':eid,'status':'shell_completed','return_code':0})
                put(evidence/'planar-cleanup.json',{'execution_id':eid,'paths':['/tmp/wrong'],'status':'passed'})
                with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=run)
                put(evidence/'planar-cleanup.json',{'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'})
                with patch.object(release.os,'killpg',return_value=None):
                    with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=run)
                with patch.object(retained_b_census,'check',return_value={'matching_commands':0,'unresolved':1}):
                    with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=run)
                with self.assertRaises(ValueError): release.live_check(evidence,trial_name,{},run=lambda *a,**k:SimpleNamespace(returncode=0,stdout='owned'))
                (registry/(hashlib.sha256((prefix+'__env').encode()).hexdigest()+'.json')).unlink();seen.clear()
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
            with patch.object(release.launch,'project_dir',return_value=project),patch.object(release.launch,'reviews_root',return_value=reviews),patch.object(release.launch,'jobs_root',return_value=jobs):
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
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): selected_context=release.launch.context()
            with patch.object(release.launch,'context',return_value=selected_context),patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs),patch.object(release,'verify',return_value={'trial_name':'trial','trial':{}}),patch.object(release,'live_check',side_effect=swapped),patch.object(release,'resolver_identity',return_value={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}):
                with codex_run.admission_scope() as records: (records/(eid+'.start.json')).write_text(json.dumps({'case':'clean-retained-a-continuation','execution_id':eid}))
                with self.assertRaisesRegex(ValueError,'review bytes changed'): release.publish(evidence,review)
                self.assertFalse((records/(eid+'.resolved.json')).exists())

class ExportFailure(unittest.TestCase):
    # Copied traceback frames from the retained Harbor job.log; paths are redacted.
    LOG = b'''Failed to download artifact '/workspace/home' from service 'main' (best-effort)
Traceback (most recent call last):
  File "/redacted/artifact_handler.py", line 351, in _download_artifact
    await source_env.service_download_dir(...)
  File "/redacted/guarded_docker.py", line 331, in service_download_dir
    return await self.download_dir(source_dir, target_dir)
  File "/redacted/guarded_docker.py", line 217, in download_dir
    await verifier_handoff(...)
  File "/redacted/clean_retained_support.py", line 96, in verifier_handoff
    final=await capture.collect(...)
  File "/redacted/capture.py", line 95, in collect
    repository, repo_meta = select_repository(...)
  File "/redacted/snapshot.py", line 42, in select_repository
    if name not in GIT_DIRS: raise ValueError('unexpected repository directory')
ValueError: unexpected repository directory
'''
    def test_recorded_chain_and_mutations(self):
        release.validate_export_log(self.LOG)
        for old,new in ((b'in service_download_dir',b'in other'),(b'in verifier_handoff',b'in other'),
                        (b'in collect',b'in other'),(b'in select_repository',b'in other'),
                        (b'ValueError: unexpected repository directory',b'ValueError: other')):
            with self.subTest(old=old), self.assertRaises(ValueError): release.validate_export_log(self.LOG.replace(old,new))

    def test_rejects_disjoint_tracebacks_and_missing_context(self):
        split=self.LOG.replace(b'  File "/redacted/capture.py"',b'Traceback (most recent call last):\n  File "/redacted/capture.py"')
        no_context=self.LOG.split(b'\n',1)[1]
        unrelated=self.LOG.replace(b'  File "/redacted/capture.py"',b'RuntimeError: unrelated\n  File "/redacted/capture.py"')
        indented_split=split.replace(b'\nTraceback (most recent call last):\n  File "/redacted/capture.py"',b'\n  Traceback (most recent call last):\n  File "/redacted/capture.py"')
        indented_error=unrelated.replace(b'\nRuntimeError: unrelated',b'\n  RuntimeError: unrelated')
        for raw in (split,indented_split,no_context,unrelated,indented_error):
            with self.subTest(raw=raw),self.assertRaises(ValueError): release.validate_export_log(raw)

    def test_closed_failure_schema(self):
        with TemporaryDirectory() as tmp:
            doc=document(Path(tmp)); doc.update(schema='planar-retained-clean-account-v2',terminal_class='final-export-failed',
                final_capture='absent',verifier_handoff='absent',semantic_result='unverified',
                missing_artifacts=release.MISSING_ARTIFACTS.copy(),job_log_sha256='d'*64,
                target_reason=release.EXPORT_REASON,target_reason_present=True)
            self.assertEqual(release.parse(json.dumps(doc))['target_status'],'incomplete')
            for key,value in (('target_reason','different'),('target_status','behavioral_failure'),('semantic_result','pass'),
                              ('final_capture','present'),('job_log_sha256','bad'),('missing_artifacts',[])):
                changed=copy.deepcopy(doc); changed[key]=value
                with self.subTest(key=key),self.assertRaises(ValueError): release.parse(json.dumps(changed))

class Reconciliation(unittest.TestCase):
    def test_locked_sidecar_and_stale_tamper_fail_closed(self):
        import codex_run
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs'; jobs.mkdir(); eid='planar-t044-'+'1'*32
            evidence=jobs/eid; evidence.mkdir(); doc=document(root); doc.update(execution_id=eid,target_status='behavioral_failure')
            review=root/'review.json'; review.write_bytes(release.canonical(doc)+b'\n'); review_hash=release.digest(review.read_bytes())
            identity={'commit':'d'*40,'implementation_sha256':'e'*64}; tree='f'*40
            census={'execution_id':eid,'native_group':'absent','matching_commands':0,'unresolved':0}
            census_hash=release.digest(release.canonical(census))
            def git(args,**kw):
                if len(args)==3 and args[1]=='rev-parse' and args[2].endswith('^{tree}'): value=(tree+'\n').encode()
                elif args[1:4]==['diff','--name-only','-z']:
                    value=(b'evals/oracle.py\0' if args[5]==release._DRIFT_ORACLE else b'evals/account.py\0' if args[4]==release._DRIFT_ORACLE else b'evals/account.py\0evals/oracle.py\0')
                elif args[1:3]==['merge-base','--is-ancestor']: value=b''
                elif len(args)==3 and args[1]=='show': value=b'reviewed source'
                else: raise AssertionError(args)
                return SimpleNamespace(stdout=value)
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(root)}): selected_context=release.launch.context()
            with patch.object(release.launch,'context',return_value=selected_context),patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs), \
                 patch.object(release,'verify',return_value={'trial_name':'trial','trial':{}}),patch.object(release,'live_check',return_value=census), \
                 patch.object(release,'resolver_identity',return_value=identity),patch.object(release.subprocess,'run',side_effect=git), \
                 patch.object(release,'_DRIFT_EID',eid),patch.object(release,'_DRIFT_REVIEW',review_hash), \
                 patch.object(release,'_DRIFT_OLD',doc['resolver_commit']),patch.object(release,'_DRIFT_OLD_IMPL',doc['resolver_implementation_sha256']):
                with codex_run.admission_scope() as records:
                    self.assertEqual(records,root/'planar-account-admission-v1'/'records')
                    start=records/(eid+'.start.json'); start.write_text(json.dumps({'case':'clean-retained-a-continuation','execution_id':eid}))
                    census_path=records/(eid+'.retained-clean-census-'+census_hash+'.json'); census_path.write_bytes(release.canonical(census)+b'\n')
                    marker={'schema':'planar-retained-clean-account-marker-v1','type':'retained-clean-account-v1','execution_id':eid,'review_path':str(review),'review_sha256':review_hash,'census_path':str(census_path),'census_sha256':census_hash,'review_binding':doc}
                    marker_path=records/(eid+'.resolved.json'); marker_path.write_bytes(release.canonical(marker)+b'\n'); marker_hash=release.digest(marker_path.read_bytes())
                expected={'schema':'planar-retained-clean-reconciliation-v1','execution_id':eid,'marker_sha256':marker_hash,'review_sha256':review_hash,'old_commit':doc['resolver_commit'],'old_implementation_sha256':doc['resolver_implementation_sha256'],'current_commit':identity['commit'],'current_tree':tree,'current_implementation_sha256':identity['implementation_sha256'],'scope':'account-only','source_status':'behavioral_failure','source_reward':0,'target_status':'behavioral_failure','target_reward':0}
                oracle=[{'path':'evals/oracle.py','sha256':release.digest(b'reviewed source')}]
                account=[{'path':'evals/account.py','sha256':release.digest(b'reviewed source')}]
                source={'schema':'planar-retained-clean-source-review-v1','decision':'approve','independent_review':True,'reviewer':'independent','reviewed_at':'2026-11-17T00:00:00+00:00','old_commit':doc['resolver_commit'],'oracle_commit':release._DRIFT_ORACLE,'current_commit':identity['commit'],'current_tree':tree,'oracle_diff':oracle,'account_diff':account}
                source_path=root/'resolver-drift-source-review-v1.json'; source_path.write_bytes(release.canonical(source)+b'\n')
                approval={'schema':'planar-retained-clean-reconciliation-approval-v1','decision':'approve-account-reconciliation','independent_review':True,'reviewer':'independent','reviewed_at':'2026-11-17T00:00:00+00:00','expected_sidecar':expected,'diff':account+oracle,'oracle_diff':oracle,'account_diff':account,'source_review_sha256':release.digest(source_path.read_bytes())}
                approval_path=root/'approval.json'; approval_path.write_bytes(release.canonical(approval)+b'\n')
                with patch.object(release,'_DRIFT_MARKER',marker_hash):
                    original_source=source_path.read_bytes(); source_path.unlink()
                    with self.assertRaises(FileNotFoundError): release.publish_reconciliation(evidence,approval_path)
                    source_path.write_bytes(original_source+b' ')
                    with self.assertRaises(ValueError): release.publish_reconciliation(evidence,approval_path)
                    source_path.write_bytes(original_source)
                    original_approval=approval_path.read_bytes()
                    bad=copy.deepcopy(approval); del bad['source_review_sha256']; approval_path.write_bytes(release.canonical(bad)+b'\n')
                    with self.assertRaises(ValueError): release.publish_reconciliation(evidence,approval_path)
                    bad=copy.deepcopy(approval); bad['account_diff']=oracle; approval_path.write_bytes(release.canonical(bad)+b'\n')
                    with self.assertRaises(ValueError): release.publish_reconciliation(evidence,approval_path)
                    approval_path.write_bytes(original_approval)
                    with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    with patch.object(release.os,'link',side_effect=OSError('fault')):
                        with self.assertRaises(OSError): release.publish_reconciliation(evidence,approval_path)
                    with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    self.assertEqual(release.publish_reconciliation(evidence,approval_path)['account'],'resolved')
                    sidecar=records/(eid+'.retained-clean-reconciliation-v1.json'); original=sidecar.read_bytes()
                    with codex_run.admission_scope() as locked: self.assertEqual(codex_run.shared_pending(locked),[])
                    with self.assertRaises(FileExistsError): release.publish_reconciliation(evidence,approval_path)
                    with patch.object(release,'resolver_identity',return_value={'commit':'9'*40,'implementation_sha256':'e'*64}):
                        with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    with patch.object(release,'live_check',side_effect=ValueError('resource active')):
                        with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    with patch.object(release,'verify',side_effect=ValueError('evidence tampered')):
                        with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    value=json.loads(original); value['target_reward']=False; sidecar.write_bytes(release.canonical(value)+b'\n')
                    with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    sidecar.unlink(); sidecar.symlink_to(approval_path)
                    with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    sidecar.unlink(); sidecar.write_bytes(original)
                    self.assertFalse(release.validate_published(start,marker_path,marker))
                    original_marker=marker_path.read_bytes(); marker_path.write_bytes(original_marker+b' ')
                    with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
                    marker_path.write_bytes(original_marker)
                    approval['diff'][0]['sha256']='0'*64; approval_path.write_bytes(release.canonical(approval)+b'\n')
                    with codex_run.admission_scope() as locked: self.assertIn(start,codex_run.shared_pending(locked))
