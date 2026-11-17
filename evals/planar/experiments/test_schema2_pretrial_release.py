import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
import codex_run
import schema2_pretrial_release as release


class PretrialPair(TestCase):
    def test_dual_marker_locks_fault_and_revalidation(self):
        with TemporaryDirectory() as tmp:
            project=Path(tmp).resolve(); reviews=project/'reviews'; reviews.mkdir(); root=reviews/'experiment'; root.mkdir(); rid='a'*32; folder=root/'executions'/rid; folder.mkdir(parents=True); scope=reviews/'planar-account-admission-v1'; records=scope/'records'; records.mkdir(parents=True)
            (folder/'terminal.json').write_text(json.dumps({'status':'incomplete','reason':'portable native provenance or cleanup unconfirmed'}))
            start=records/(rid+'.start.json'); start.write_text(json.dumps({'execution_id':rid,'experiment':str(root),'runnable_contract':'planar-schema2-runnable-v1'})); doc={'schema':'planar-schema2-pretrial-release-v1','decision':'release-account','phase':'before-native-trial','terminal_class':'incomplete','native_job_id':None,'native_trial_id':None,'verifier_result':None,'model_result':None,'experiment_root':str(root),'experiment_identity':'b'*64,'reservation_id':rid,'gate_path':str(reviews/'gate'),'gate_sha256':'c'*64,'preparation_path':str(reviews/'prep'),'independent_audit_path':str(reviews/'audit'),'independent_audit_sha256':'d'*64,'historical_commit':'e'*40,'historical_tree':'f'*40,'historical_implementation_sha256':'1'*64,'resolver_commit':'2'*40,'resolver_implementation_sha256':'3'*64,'artifacts':{'/fixture':'4'*64},'reviewer':'independent','independence_basis':'review','reviewed_at':'2026-01-01T00:00:00+00:00','independent_review':True}; review=reviews/'review.json'; review.write_bytes(release.canonical(doc))
            verified={'root':root,'evidence':reviews/'unused','folder':folder,'process_pid':123,'run_id':'planar-t044-'+rid}; census={'execution_id':'planar-t044-'+rid,'native_group':'absent','matching_commands':0,'unresolved':0}; identity={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}
            with patch.object(release.launch,'PROJECT',project),patch.object(release.launch,'reviews_root',return_value=reviews),patch.object(release.launch,'jobs_root',return_value=reviews/'jobs'),patch.object(release,'verify',return_value=verified),patch.object(release,'live_check',return_value=census),patch.object(release,'resolver_identity',return_value=identity):
                # Swapping the independently reviewed bytes during the locked
                # cleanup check must not publish either marker.
                original_review=review.read_bytes()
                def swap_during_check(_verified):
                    review.write_bytes(original_review+b' ')
                    return census
                with patch.object(release,'live_check',side_effect=swap_during_check):
                    with self.assertRaisesRegex(ValueError,'review changed before publication'): release.publish(review)
                self.assertFalse((folder/'account-stop-resolved.json').exists()); self.assertFalse((records/(rid+'.resolved.json')).exists())
                review.write_bytes(original_review)
                # Failure after the local marker leaves the account pending.
                original=release.schema2.write_new
                def fail_shared(path,value):
                    if Path(path).name==rid+'.resolved.json': raise OSError('fault before shared marker')
                    return original(path,value)
                with patch.object(release.schema2,'write_new',side_effect=fail_shared):
                    with self.assertRaises(OSError): release.publish(review)
                self.assertTrue((folder/'account-stop-resolved.json').exists()); self.assertFalse((records/(rid+'.resolved.json')).exists())
                with codex_run.admission_scope() as locked: self.assertEqual(len(codex_run.shared_pending(locked)),1)
                with self.assertRaisesRegex(ValueError,'partial resolution'): release.publish(review)
                # A fresh isolated fixture permits the positive pair; remove only
                # the test-owned local partial marker between subcases.
                (folder/'account-stop-resolved.json').unlink()
                self.assertEqual(release.publish(review)['account'],'resolved')
                self.assertEqual(len(codex_run.shared_pending(records)),1)
                with codex_run.admission_scope() as locked: self.assertEqual(codex_run.shared_pending(locked),[])
                self.assertEqual(release.publish(review)['account'],'resolved')
                with patch.object(release,'live_check',side_effect=ValueError('active')):
                    with codex_run.admission_scope() as locked: self.assertEqual(len(codex_run.shared_pending(locked)),1)
                original_review=review.read_bytes(); review.write_bytes(original_review+b' ')
                with codex_run.admission_scope() as locked: self.assertEqual(len(codex_run.shared_pending(locked)),1)
                review.write_bytes(original_review)
                marker=records/(rid+'.resolved.json'); data=json.loads(marker.read_text()); data['review_binding']['independent_review']=1; marker.write_bytes(release.canonical(data))
                with codex_run.admission_scope() as locked: self.assertEqual(len(codex_run.shared_pending(locked)),1)

    def test_schema_rejects_fabricated_result_and_bool(self):
        d={k:'x' for k in release.FIELDS}
        d.update({'schema':'planar-schema2-pretrial-release-v1','decision':'release-account','phase':'before-native-trial','terminal_class':'incomplete','native_job_id':None,'native_trial_id':None,'verifier_result':None,'model_result':None,'experiment_root':'/x/experiment','experiment_identity':'1'*64,'reservation_id':'a'*32,'gate_path':'/x/gate','gate_sha256':'2'*64,'preparation_path':'/x/prep','independent_audit_path':'/x/audit','independent_audit_sha256':'3'*64,'historical_commit':'a'*40,'historical_tree':'b'*40,'historical_implementation_sha256':'4'*64,'resolver_commit':'c'*40,'resolver_implementation_sha256':'5'*64,'artifacts':{'/x/file':'6'*64},'reviewer':'reviewer','independence_basis':'independent','reviewed_at':'2026-01-01T00:00:00+00:00','independent_review':True})
        self.assertIsNone(release.parse(release.canonical(d))['native_job_id'])
        for field,bad in (('native_job_id','invented'),('verifier_result',0),('model_result',False),('independent_review',1)):
            changed=dict(d); changed[field]=bad
            with self.subTest(field=field), self.assertRaises(ValueError): release.parse(release.canonical(changed))

class RetainedPretrialEvidence(TestCase):
    @patch.dict('os.environ',{'PLANAR_EVALS_OUTPUT_ROOT':'/Users/mn/.planar/projects/planar/reviews/t046-schema2-native-proof-prep-20261117/output'})
    def test_source_hashed_copies_and_static_mutations(self):
        import subprocess
        b=Path('/Users/mn/.planar/projects/planar/reviews/t046-schema2-native-proof-prep-20261117'); root=b/'experiment'; rid='2b4416f4dda34ca39fc934a5762ad42b'; records=b/'output/planar-account-admission-v1/records'; e=b/'output/jobs'/('planar-t044-'+rid); folder=root/'executions'/rid; gate=b/'output/review-gate-approved.json'; prep=b/'output/preparation.json'; audit=release.launch.PROJECT/'reviews/t046-schema2-first-failure-independent-audit-20261117.md'; g=json.loads(gate.read_text()); marker=b/'output/harbor-t044-consumed/258cbcbfb6a67b1799d58b610e50b438fca7b70bb9ad56098c236e97f51b6cbb'; historical='2b1ebfb936c5406b44a521c1f23b82d583f6f951'; resolver='f48de4a'; tree=lambda rev:subprocess.check_output(['git','rev-parse',rev+'^{tree}'],text=True).strip(); commit=lambda rev:subprocess.check_output(['git','rev-parse',rev],text=True).strip(); resolver=commit(resolver)
        paths=[root/'manifest.json',root/'reservations'/(rid+'.json'),gate,marker,records/(rid+'.start.json'),prep,prep.with_name(prep.name+'.build.log'),audit,*[folder/z for z in release.EXEC_FILES],*[e/z for z in release.EVIDENCE_FILES]]
        doc={'experiment_root':str(root),'experiment_identity':g['experiment_identity'],'reservation_id':rid,'gate_path':str(gate),'gate_sha256':release.h(gate.read_bytes()),'preparation_path':str(prep),'independent_audit_path':str(audit),'independent_audit_sha256':release.h(audit.read_bytes()),'historical_commit':historical,'historical_tree':tree(historical),'historical_implementation_sha256':g['implementation_sha256'],'resolver_commit':resolver,'resolver_implementation_sha256':release.historical_identity(resolver,tree(resolver)),'artifacts':{str(p):release.h(p.read_bytes()) for p in paths}}
        with TemporaryDirectory() as temp:
            copies={}
            for i,p in enumerate(paths):
                dst=Path(temp)/str(i); dst.write_bytes(p.read_bytes()); copies[str(p)]=dst
            manifest={'source_sha256':doc['artifacts'],'copy_sha256':{str(p):release.h(copies[str(p)].read_bytes()) for p in paths}}
            self.assertEqual(manifest['source_sha256'],manifest['copy_sha256'])
            self.assertEqual(release.verify(doc,records)['run_id'],'planar-t044-'+rid)
            original_read=release.read_safe
            def copy_read(path,root,*args,**kwargs):
                copy=copies.get(str(path))
                return copy.read_bytes() if copy is not None else original_read(path,root,*args,**kwargs)
            with patch.object(release,'read_safe',side_effect=copy_read):
                original=copies[str(gate)].read_bytes(); copies[str(gate)].write_bytes(original+b' ')
                with self.assertRaises(ValueError): release.verify(doc,records)
                copies[str(gate)].write_bytes(original)
                original=copies[str(e/'harbor-process.json')].read_bytes(); altered=json.loads(original); altered['pid']=True; copies[str(e/'harbor-process.json')].write_text(json.dumps(altered))
                with self.assertRaisesRegex(ValueError,'process mismatch'): release.verify(doc,records)
                copies[str(e/'harbor-process.json')].write_bytes(original)
                original=copies[str(audit)].read_bytes(); copies[str(audit)].write_bytes(original+b' ')
                with self.assertRaisesRegex(ValueError,'audit mismatch'): release.verify(doc,records)
                copies[str(audit)].write_bytes(original)
            original_record=release.schema2.read_record
            def altered_record(path):
                value=original_record(path)
                if Path(path)==root/'reservations'/(rid+'.json'): value['execution_id']='f'*32
                return value
            with patch.object(release.schema2,'read_record',side_effect=altered_record):
                with self.assertRaisesRegex(ValueError,'reservation authority mismatch'): release.verify(doc,records)
            def changed_terminal(path):
                value=original_record(path)
                if Path(path)==folder/'terminal.json': value['status']='completed'
                return value
            with patch.object(release.schema2,'read_record',side_effect=changed_terminal):
                with self.assertRaisesRegex(ValueError,'terminal or stop mismatch'): release.verify(doc,records)
            tampered=dict(doc, historical_implementation_sha256='0'*64)
            with self.assertRaisesRegex(ValueError,'historical source mismatch'): release.verify(tampered,records)
            actual_iter=Path.iterdir
            def unexpected(path):
                values=list(actual_iter(path))
                if path==e: values.append(e/'native-trial-start.json')
                return iter(values)
            with patch.object(Path,'iterdir',unexpected):
                with self.assertRaisesRegex(ValueError,'unexpected pretrial artifact'): release.verify(doc,records)

class PretrialLive(TestCase):
    def test_resource_group_and_census_fail_closed(self):
        from types import SimpleNamespace
        with TemporaryDirectory() as temp:
            evidence=Path(temp)/('planar-t044-'+'b'*32)
            evidence.mkdir(); verified={'evidence':evidence,'run_id':evidence.name,'process_pid':123}
            absent=SimpleNamespace(returncode=0,stdout='')
            with patch.object(release.os,'killpg',side_effect=ProcessLookupError),patch.object(release.subprocess,'run',return_value=absent) as docker,patch.object(release.retained_b_census,'check',return_value={'matching_commands':0,'unresolved':0}):
                self.assertEqual(release.live_check(verified)['native_group'],'absent'); self.assertEqual(docker.call_count,3)
                with patch.object(release.subprocess,'run',return_value=SimpleNamespace(returncode=1,stdout='')):
                    with self.assertRaisesRegex(ValueError,'resource absence'): release.live_check(verified)
                with patch.object(release.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout='container')):
                    with self.assertRaisesRegex(ValueError,'resource absence'): release.live_check(verified)
                with patch.object(release.retained_b_census,'check',return_value={'matching_commands':0,'unresolved':1}):
                    with self.assertRaisesRegex(ValueError,'census'): release.live_check(verified)
                with patch.object(release.os,'killpg',return_value=None):
                    with self.assertRaisesRegex(ValueError,'group active'): release.live_check(verified)
                (evidence/'recovery-required').touch()
                with self.assertRaisesRegex(ValueError,'recovery'): release.live_check(verified)
