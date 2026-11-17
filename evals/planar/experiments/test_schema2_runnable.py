import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch
from contextlib import contextmanager, redirect_stdout
import io
import fcntl
import threading
import shutil
import hashlib
import schema2_execution
import codex_run
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
import launch
from harbor.models.job.config import JobConfig
from harbor.models.task.task import Task
import per_variant as pv
import schema2
import schema2_projection as projection
import schema2_runnable as runnable
import schema2_reporting
import test_schema2 as fixture

def pair(path): return {'raw':str(path),'resolved':str(path.resolve())}

class RunnableReview(unittest.TestCase):
    def setUp(self):
        fixture.SchemaTwo.setUp(self)
        self.reviews=self.base/'reviews'; self.jobs=self.base/'jobs'; self.reviews.mkdir(); self.jobs.mkdir()
        roots={'experiment':pair(self.root),'reviews':pair(self.reviews),'jobs':pair(self.jobs),'consumed':pair(self.reviews/'harbor-t044-consumed')}
        case=self.root/'variants/a/case-instance'
        dummy='f'*32
        reference=JobConfig.model_validate(launch.config_for('planar-t044-'+dummy,self.jobs/('planar-t044-'+dummy),case,'medium'))
        _,normalized=projection.normalize(reference,execution_id=dummy,evidence_raw=str(self.jobs/('planar-t044-'+dummy)),case_raw=str(case),effort='medium')
        m=self.manifest
        m['runnable_contract']=runnable.CONTRACT
        m['configuration']['config']['native_config']={'policy_version':1,'harbor_version':'0.26.0','projection_version':1,'normalized_job_config':normalized,'roots':roots}
        m['configuration_sha256']=schema2.object_hash(m['configuration'])
        for v in m['variants']:
            v['configuration']={**m['configuration'],'task':{'case':'planar/native-lineage-portable','contract_version':2},'variant_id':v['variant_id'],'effort':v['effort'],'image_id':v['image_id'],'verifier_image_id':v['verifier_image_id'],'host_version':v['host_version']}
            v['configuration_sha256']=schema2.object_hash(v['configuration'])
        m['identity']=schema2.object_hash({k:v for k,v in m.items() if k!='identity'})
        (self.root/'manifest.json').write_bytes(pv.canonical(m)); self.identity=m['identity']
        eid='e'*32; v=m['variants'][0]
        self.r={'schema_version':2,'state':'planned','reservation_id':eid,'execution_id':eid,'experiment_identity':self.identity,'logical_id':'a:1','variant_id':'a','repetition':1,'predecessor_execution_id':None,'retry_reason_sha256':None,**{key:v[key] for key in schema2.IDENTITIES},'configuration_sha256':v['configuration_sha256'],'implementation_sha256':m['implementation_sha256'],'image_id':v['image_id'],'verifier_image_id':v['verifier_image_id'],'host_version':v['host_version'],'account_scope_id':m['configuration']['account_scope_id'],'canonical_output_root':m['configuration']['canonical_output_root'],'expires_at':fixture.timestamp(600)}
        self.job=JobConfig.model_validate(launch.config_for('planar-t044-'+eid,self.jobs/('planar-t044-'+eid),case,'medium'))
        full,_=projection.normalize(self.job,execution_id=eid,evidence_raw=str(self.jobs/('planar-t044-'+eid)),case_raw=str(case),effort='medium')
        self.original=Task(case).config.verifier.environment
        v=m['variants'][0]; self.evidence=self.base/'preparation.json'
        prep={'case_instance_sha256':v['case_instance_sha256'],'trusted_inputs_sha256':v['trusted_inputs_sha256'],'verifier_code_sha256':v['verifier_code_sha256'],'build_context_sha256':pv.tree_hash(case/'tests'),'base_image_id':'sha256:'+'f'*64,'verifier_image_id':v['verifier_image_id'],'agent_image_id':v['image_id'],'preparation_evidence_sha256':None}
        self.evidence.with_name(self.evidence.name+'.build.log').write_bytes(b'synthetic build\n')
        evidence_record={'schema_version':1,'build_method':'docker-build-no-pull-network-none-v1','build_log_sha256':schema2.h(b'synthetic build\n'),**{k:value for k,value in prep.items() if k!='preparation_evidence_sha256'}}
        self.evidence.write_bytes(pv.canonical(evidence_record)); prep['preparation_evidence_sha256']=schema2.h(self.evidence.read_bytes())
        bindings={**roots,'candidate':pair(self.root/'variants/a/candidate'),'case_instance':pair(case),'evidence':pair(self.jobs/('planar-t044-'+eid))}
        binding={'binding_version':1,'projection_version':1,'job_config':full,'effective_verifier_environment':projection.project(projection.effective_verifier_environment(self.original,v['verifier_image_id'])),'path_bindings':bindings,'caps':m['configuration']['config']['caps'],'verifier_preparation':prep}
        binding['concrete_job_config_sha256']=runnable.digest(binding)
        self.r.update({'runnable_contract':runnable.CONTRACT,'concrete_binding':binding})
        self.path=self.root/'reservations'/f'{eid}.json'; self.path.write_bytes(pv.canonical(self.r))
        self.gate={k:self.r[k] for k in schema2.GATE_FIELDS if k in self.r}
        self.gate.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':schema2.h(self.path.read_bytes()),'reviewer':'independent','report':'synthetic','reviewed_at':fixture.timestamp(-1),'runnable_contract':runnable.CONTRACT,'concrete_job_config_sha256':binding['concrete_job_config_sha256']})
    def check(self): return runnable.check(self.root,self.identity,self.r['reservation_id'],self.gate,job=self.job,original_verifier_environment=self.original,preparation_evidence=self.evidence)
    def test_explicit_runnable_plan_and_wrong_policy(self):
        out=self.base/'runnable-plan'; reviews=self.reviews; jobs=self.jobs
        roots={'experiment':pair(out),'reviews':pair(reviews),'jobs':pair(jobs),'consumed':pair(reviews/'harbor-t044-consumed')}
        fake='0'*32; case=out/'variants/a/case-instance'; evidence=jobs/('planar-t044-'+fake)
        job=JobConfig.model_validate(launch.config_for('planar-t044-'+fake,evidence,case,'medium'))
        policy=runnable.derive_policy(job,execution_id=fake,evidence_raw=str(evidence),case_raw=str(case),effort='medium',roots=roots)
        variants=[{'id':'a','candidate_source':str(self.base/'a'),'effort':'medium','image_id':Task(fixture.TEMPLATE).config.environment.docker_image,'verifier_image_id':'sha256:'+'b'*64,'host_version':'codex-cli 0.169.0'}]
        config=copy.deepcopy(self.manifest['configuration']['config']); config['variant_efforts']={'a':'medium'}; config['native_config']=policy; config['caps']['session_seconds']=600; config['caps']['verifier_seconds']=60; config['caps']['trial_seconds']=700
        args={'template':fixture.TEMPLATE,'reviewed_template_sha256':pv.tree_hash(fixture.TEMPLATE),'variants':variants,'repetitions':1,'config':config,'implementation_sha256':'d'*64,'deadline_at':fixture.timestamp(3000),'account_scope_id':str((reviews/'planar-account-admission-v1').resolve()),'runnable_contract':runnable.CONTRACT}
        with patch.object(launch,'reviews_root',return_value=reviews), patch.object(launch,'jobs_root',return_value=jobs):
            impossible=copy.deepcopy(args); impossible['config']['caps']['job_seconds']=1
            with self.assertRaisesRegex(schema2.Invalid,'wall-time cap'):
                schema2.initialize(out,**impossible)
            self.assertFalse(out.exists())
            manifest=schema2.initialize(out,**args)
            self.assertEqual(manifest['runnable_contract'],runnable.CONTRACT)
            self.assertEqual(schema2.validate(out,manifest['identity'])['identity'],manifest['identity'])
            with self.assertRaises(schema2.Invalid): schema2.reserve(out,manifest['identity'],'a:1',expires_at=fixture.timestamp(600))
            v=manifest['variants'][0]; instance=out/'variants/a/case-instance'
            prep={'schema_version':1,'build_method':'docker-build-no-pull-network-none-v1','build_log_sha256':schema2.h(b'synthetic build\n'),'case_instance_sha256':v['case_instance_sha256'],'trusted_inputs_sha256':v['trusted_inputs_sha256'],'verifier_code_sha256':v['verifier_code_sha256'],'build_context_sha256':pv.tree_hash(instance/'tests'),'base_image_id':'sha256:'+'f'*64,'verifier_image_id':v['verifier_image_id'],'agent_image_id':v['image_id']}
            source=reviews/'preparation.json'; source.with_name(source.name+'.build.log').write_bytes(b'synthetic build\n'); source.write_bytes(pv.canonical(prep))
            from types import SimpleNamespace
            def inspect(argv,**kwargs): return SimpleNamespace(returncode=0,stdout=argv[-1]+'\n')
            with patch('shutil.which',return_value='/usr/bin/docker'), patch('subprocess.run',side_effect=inspect):
                reserved=runnable.reserve(out,manifest['identity'],'a:1',expires_at=fixture.timestamp(1500),preparation_evidence=source)
            self.assertEqual(reserved['runnable_contract'],runnable.CONTRACT)
            self.assertEqual(reserved['reservation_id'],reserved['execution_id'])
            self.assertEqual(len(list((out/'executions').iterdir())),0)
            # A paused/continued experiment retains its original deadline;
            # advancing the clock cannot create a new runnable reservation.
            import datetime as dt
            original_deadline=manifest['deadline_at']; base_now=dt.datetime.now(dt.timezone.utc)
            class Later(dt.datetime):
                @classmethod
                def now(cls,tz=None): return base_now+dt.timedelta(seconds=4000)
            with patch.object(dt,'datetime',Later):
                with self.assertRaises(schema2.Invalid): runnable.reserve(out,manifest['identity'],'a:1',expires_at=fixture.timestamp(100),preparation_evidence=source)
            self.assertEqual(schema2.read_record(out/'manifest.json')['deadline_at'],original_deadline)
            report=schema2_reporting.report(out,manifest['identity'])
            self.assertEqual(report['runnable_contract'],runnable.CONTRACT)
            self.assertEqual(report['groups'][0]['primary_not_started'],1)
            gate={k:reserved[k] for k in schema2.GATE_FIELDS if k in reserved}
            gate.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':schema2.h((out/'reservations'/f"{reserved['reservation_id']}.json").read_bytes()),'reviewer':'synthetic','report':'synthetic','reviewed_at':fixture.timestamp(-1),'runnable_contract':runnable.CONTRACT,'concrete_job_config_sha256':reserved['concrete_binding']['concrete_job_config_sha256']})
            self.assertEqual(runnable.check_gate(out,manifest['identity'],reserved['reservation_id'],gate,preparation_evidence=source)['authorization'],'not granted')
            wrong_gate=dict(gate,concrete_job_config_sha256='0'*64)
            with self.assertRaises(schema2.Invalid): runnable.check_gate(out,manifest['identity'],reserved['reservation_id'],wrong_gate,preparation_evidence=source)
            gate_path=reviews/'runnable-gate.json'; gate_path.write_bytes(pv.canonical(gate))
            # Exercise the real portable launcher up to, but never into, auth.
            class StopBeforeAuth:
                def resolve(self,**kwargs): raise RuntimeError('auth boundary reached')
            with patch.object(schema2_execution,'verify_activated',return_value={'reservation':reserved}), patch('shutil.which',return_value='/mock/docker'), patch('subprocess.run',side_effect=inspect), patch.object(launch.platform,'platform',return_value='test'):
                with self.assertRaisesRegex(RuntimeError,'auth boundary reached'):
                    launch.execute(StopBeforeAuth(),gate_path,'native-lineage-portable',candidate_root=out/'variants/a/candidate',case_root=instance,trial_deadline=700,execution_id='planar-t044-'+reserved['execution_id'],effort='medium',absolute_deadline=reserved['expires_at'],emit=False,schema2_context={'root':str(out),'identity':manifest['identity'],'reservation_id':reserved['execution_id'],'preparation_evidence':str(source)})
            changed=copy.deepcopy(reserved); changed['concrete_binding']['job_config']['debug']=True
            with patch.object(schema2_execution,'verify_activated',return_value={'reservation':changed}), patch('shutil.which',return_value='/mock/docker'), patch('subprocess.run',side_effect=inspect), patch.object(launch.platform,'platform',return_value='test'):
                with self.assertRaisesRegex(ValueError,'mismatch before auth'):
                    launch.execute(StopBeforeAuth(),gate_path,'native-lineage-portable',candidate_root=out/'variants/a/candidate',case_root=instance,trial_deadline=700,execution_id='planar-t044-'+reserved['execution_id'],effort='medium',absolute_deadline=reserved['expires_at'],emit=False,schema2_context={'root':str(out),'identity':manifest['identity'],'reservation_id':reserved['execution_id'],'preparation_evidence':str(source)})
            scope=reviews/'planar-account-admission-v1'; records=scope/'records'; records.mkdir(parents=True)
            @contextmanager
            def admission():
                with (scope/'lock').open('a+') as account_lock:
                    fcntl.flock(account_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
                    yield records
            # Fault-inject after each of the first two durable admission writes.
            eid=reserved['execution_id']; folder=out/'executions'/eid
            shared=records/(eid+'.start.json'); closed=records/(eid+'.closed.json')
            marker_hash=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
            marker=reviews/'harbor-t044-consumed'/marker_hash
            real_write=schema2.write_new
            def now_inspect(argv,**kwargs):
                return SimpleNamespace(returncode=0,stdout=('codex-cli 0.169.0\n' if argv[-1]=='--version' else argv[-1]+'\n'))
            for boundary in ('shared','local'):
                target=shared if boundary=='shared' else folder/'start.json'
                def fail_after_write(path,value):
                    real_write(path,value)
                    if Path(path)==target: raise OSError('synthetic crash after '+boundary)
                with self.subTest(crash_after=boundary), patch.object(codex_run,'admission_scope',admission), patch.object(schema2_execution,'implementation_identity',return_value='d'*64), patch('shutil.which',side_effect=lambda name:'/mock/'+name), patch('subprocess.run',side_effect=now_inspect), patch.object(launch,'execute') as no_launch:
                    with patch.object(schema2,'write_new',side_effect=fail_after_write):
                        with self.assertRaises(OSError): schema2_execution.activate_runnable(out,manifest['identity'],eid,gate_path,preparation_evidence=source,_dispatch=lambda:None)
                    self.assertTrue(shared.is_file()); self.assertFalse(marker.exists())
                    self.assertEqual((folder/'start.json').exists(),boundary=='local')
                    original_shared=shared.read_bytes(); original_gate=gate_path.read_bytes()
                    alternate=reviews/'other-gate.json'; alternate.write_bytes(pv.canonical({**gate,'reviewer':'different'}))
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],eid,alternate))
                    self.assertFalse(marker.exists()); self.assertEqual(shared.read_bytes(),original_shared)
                    if boundary=='shared':
                        # A historical shared start without the bound gate is
                        # ambiguous and must remain blocked for operator audit.
                        shared.write_bytes(pv.canonical({'experiment':str(out.resolve()),'execution_id':eid}))
                        self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path))
                        self.assertFalse(marker.exists()); self.assertFalse(folder.exists())
                        shared.write_bytes(original_shared)
                        marker.parent.mkdir(mode=0o700,exist_ok=True); marker.write_text('reserved\n')
                        self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path))
                        self.assertFalse(folder.exists()); self.assertFalse(closed.exists()); marker.unlink()
                    unexpected=jobs/('planar-t044-'+eid); unexpected.mkdir()
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path))
                    self.assertFalse(marker.exists()); self.assertFalse(closed.exists())
                    unexpected.rmdir()
                    recovered=schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path)
                    self.assertEqual(recovered['status'],'incomplete'); self.assertFalse(recovered['stop_account'])
                    self.assertTrue(marker.is_file()); self.assertTrue(closed.is_file())
                    self.assertFalse((folder/'dispatch.json').exists()); self.assertFalse((jobs/('planar-t044-'+eid)).exists())
                    self.assertEqual(shared.read_bytes(),original_shared); self.assertEqual(gate_path.read_bytes(),original_gate)
                    self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path),recovered)
                    no_launch.assert_not_called()
                # Reset only the isolated synthetic fixture for the next fault.
                shutil.rmtree(folder); shared.unlink(); closed.unlink(); marker.unlink(); alternate.unlink()
            # Real paused dispatcher: recovery must fail to take the shared
            # lock after gate consumption but before dispatch is durably written.
            paused=threading.Event(); proceed=threading.Event(); real_reconcile=schema2_execution.reconcile_runnable
            def pause_dispatch(path,value):
                if Path(path)==folder/'dispatch.json':
                    paused.set()
                    if not proceed.wait(5): raise RuntimeError('dispatch pause timed out')
                return real_write(path,value)
            def complete_reconciliation(root_arg,identity_arg,eid_arg,gate_arg):
                real_write(folder/'terminal.json',{'status':'pass','reason':'synthetic reconciled'})
                real_write(closed,{'status':'pass'})
                return {'status':'pass','reason':'synthetic reconciled','stop_account':False}
            with patch.object(codex_run,'admission_scope',admission), patch.object(schema2_execution,'implementation_identity',return_value='d'*64), patch('shutil.which',side_effect=lambda name:'/mock/'+name), patch('subprocess.run',side_effect=now_inspect), patch.object(schema2,'write_new',side_effect=pause_dispatch), patch.object(launch,'execute',return_value={'status':'completed'}), patch.object(schema2_execution,'reconcile_runnable',side_effect=complete_reconciliation):
                outcomes=[]; failures=[]
                def paused_dispatcher():
                    try: outcomes.append(schema2_execution.execute_runnable(out,manifest['identity'],eid,'/nonexistent-auth',gate_path,preparation_evidence=source))
                    except Exception as exc: failures.append(exc)
                thread=threading.Thread(target=paused_dispatcher); thread.start()
                try:
                    self.assertTrue(paused.wait(5)); self.assertTrue(marker.exists()); self.assertFalse((folder/'dispatch.json').exists())
                    self.assertIsNone(real_reconcile(out,manifest['identity'],eid,gate_path))
                    self.assertFalse(closed.exists()); self.assertFalse((folder/'terminal.json').exists()); self.assertFalse((folder/'account-stop.json').exists())
                finally:
                    proceed.set(); thread.join(10)
                self.assertFalse(thread.is_alive()); self.assertEqual(failures,[])
                self.assertEqual(outcomes[0]['status'],'pass'); self.assertTrue(closed.is_file()); self.assertFalse((folder/'account-stop.json').exists())
                terminal_before=(folder/'terminal.json').read_bytes()
                self.assertFalse(schema2_execution._record_unreconciled(out,eid)['stop_account'])
                self.assertEqual((folder/'terminal.json').read_bytes(),terminal_before); self.assertFalse((folder/'account-stop.json').exists())
            shutil.rmtree(folder); shared.unlink(); closed.unlink(); marker.unlink()
            # A real interrupted attempt at the same boundary releases both
            # locks; the existing pre-dispatch recovery remains idempotent.
            def crash_before_dispatch(path,value):
                if Path(path)==folder/'dispatch.json': raise OSError('synthetic crash before dispatch')
                return real_write(path,value)
            with patch.object(codex_run,'admission_scope',admission), patch.object(schema2_execution,'implementation_identity',return_value='d'*64), patch('shutil.which',side_effect=lambda name:'/mock/'+name), patch('subprocess.run',side_effect=now_inspect), patch.object(schema2,'write_new',side_effect=crash_before_dispatch), patch.object(launch,'execute') as no_launch:
                with self.assertRaises(OSError): schema2_execution.execute_runnable(out,manifest['identity'],eid,'/nonexistent-auth',gate_path,preparation_evidence=source)
                self.assertTrue(marker.exists()); self.assertFalse((folder/'dispatch.json').exists()); no_launch.assert_not_called()
            with patch.object(codex_run,'admission_scope',admission):
                recovered=schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path)
                self.assertEqual(recovered['status'],'incomplete'); self.assertFalse(recovered['stop_account'])
                self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],eid,gate_path),recovered)
            shutil.rmtree(folder); shared.unlink(); closed.unlink(); marker.unlink()
            # An expired absolute deadline at activation refuses while no
            # shared account or gate marker has been created.
            class Expired(dt.datetime):
                @classmethod
                def now(cls,tz=None): return base_now+dt.timedelta(seconds=4000)
            with patch.object(schema2,'datetime',Expired), patch.object(codex_run,'admission_scope',admission), patch.object(schema2_execution,'implementation_identity',return_value='d'*64):
                with self.assertRaises(schema2.Invalid): schema2_execution.activate_runnable(out,manifest['identity'],eid,gate_path,preparation_evidence=source,_dispatch=lambda:None)
            self.assertFalse(shared.exists()); self.assertFalse(marker.exists())
            entered=threading.Event(); release=threading.Event()
            def actual(argv,**kwargs):
                if argv[-1]=='--version':
                    entered.set()
                    if not release.wait(5): raise RuntimeError('test synchronization timed out')
                return SimpleNamespace(returncode=0,stdout=('codex-cli 0.169.0\n' if argv[-1]=='--version' else argv[-1]+'\n'))
            def fake_launch(*args,**kwargs):
                self.assertTrue((records/(reserved['execution_id']+'.start.json')).is_file())
                self.assertEqual(len(list((reviews/'harbor-t044-consumed').iterdir())),1)
                return {'status':'completed','evidence':'synthetic'}
            with patch.object(codex_run,'admission_scope',admission), patch.object(schema2_execution,'implementation_identity',return_value='d'*64), patch('shutil.which',side_effect=lambda name:'/mock/'+name), patch('subprocess.run',side_effect=actual), patch.object(launch,'execute',side_effect=fake_launch) as dispatched:
                outcomes=[]; failures=[]
                def first():
                    try: outcomes.append(schema2_execution.execute_runnable(out,manifest['identity'],reserved['execution_id'],'/nonexistent-auth',gate_path,preparation_evidence=source))
                    except Exception as exc: failures.append(exc)
                thread=threading.Thread(target=first); thread.start()
                try:
                    self.assertTrue(entered.wait(5))
                    with self.assertRaises((schema2.Invalid,BlockingIOError)):
                        schema2_execution.activate_runnable(out,manifest['identity'],reserved['execution_id'],gate_path,preparation_evidence=source,_dispatch=lambda:None)
                finally:
                    release.set(); thread.join(10)
                self.assertFalse(thread.is_alive()); self.assertEqual(failures,[]); self.assertEqual(len(outcomes),1)
                started=outcomes[0]
                self.assertEqual(started['execution_id'],reserved['reservation_id'])
                self.assertEqual(started['status'],'incomplete'); self.assertTrue(started['stop_account'])
                self.assertEqual(dispatched.call_count,1)
                self.assertTrue((records/(reserved['execution_id']+'.start.json')).is_file())
                self.assertEqual(len(list((reviews/'harbor-t044-consumed').iterdir())),1)
                with self.assertRaises(schema2.Invalid): schema2_execution.activate_runnable(out,manifest['identity'],reserved['execution_id'],gate_path,preparation_evidence=source,_dispatch=lambda:None)
                # Exercise the real CLI resolver with genuine schema-2 stop
                # records; no cleanup proof means no resolution.
                folder=out/'executions'/reserved['execution_id']; terminal_before=(folder/'terminal.json').read_bytes()
                import cli
                command=['cli','resolve-account-stop-v2',str(out),manifest['identity'],reserved['execution_id'],'--reason','operator reviewed stop','--gate',str(gate_path)]
                def resolve_cli(argv=command):
                    with patch.object(sys,'argv',argv), redirect_stdout(io.StringIO()) as captured:
                        cli.main()
                    return json.loads(captured.getvalue())
                with self.assertRaises(schema2.Invalid): resolve_cli()
                blank=command.copy(); blank[blank.index('--reason')+1]='   '
                with self.assertRaises(schema2.Invalid): resolve_cli(blank)
                run_id='planar-t044-'+reserved['execution_id']; cleanup_evidence=jobs/run_id; cleanup_evidence.mkdir()
                for name in ('watchdog-terminal.json','launcher-terminal.json'):
                    (cleanup_evidence/name).write_text(json.dumps({'execution_id':run_id,'status':'completed'}))
                marker_pending=cleanup_evidence/'recovery-required'; marker_pending.write_text('pending')
                with self.assertRaises(schema2.Invalid): resolve_cli()
                marker_pending.unlink()
                shared_path=records/(reserved['execution_id']+'.start.json'); original=shared_path.read_bytes()
                altered=schema2.read_record(shared_path); altered['experiment']='foreign'; shared_path.write_bytes(pv.canonical(altered))
                with self.assertRaises(schema2.Invalid): resolve_cli()
                shared_path.write_bytes(original)
                self.assertEqual(resolve_cli()['status'],'resolved'); self.assertEqual(resolve_cli()['status'],'resolved')
                conflicting=command.copy(); conflicting[conflicting.index('--reason')+1]='different reason'
                with self.assertRaises(schema2.Invalid): resolve_cli(conflicting)
                self.assertEqual((folder/'terminal.json').read_bytes(),terminal_before)
                self.assertTrue((records/(reserved['execution_id']+'.resolved.json')).is_file())
                self.assertNotIn(reserved['execution_id'],codex_run.pending_account_stops(out))
                self.assertEqual(codex_run.shared_pending(records),[])
                (records/(reserved['execution_id']+'.resolved.json')).unlink(); (folder/'account-stop-resolved.json').unlink(); shutil.rmtree(cleanup_evidence)
                # Reconciliation adapter must reject contradictory provenance
                # before invoking the existing native reconciler.
                # Isolated crash-before-dispatch simulation: retained start and
                # consumed marker remain, and no launcher evidence exists.
                dispatch_bytes=(folder/'dispatch.json').read_bytes()
                for name in ('dispatch.json','launcher-result.json','account-stop.json','terminal.json'): (folder/name).unlink()
                recovered=schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path)
                self.assertEqual(recovered['status'],'incomplete'); self.assertFalse(recovered['stop_account'])
                self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path),recovered)
                closed=records/(reserved['execution_id']+'.closed.json'); self.assertTrue(closed.is_file())
                counts=schema2_reporting.report(out,manifest['identity'])['groups'][0]
                self.assertEqual((counts['primary_not_started'],counts['primary_incomplete'],counts['all_attempts_incomplete']),(1,1,1))
                # Restore this throwaway fixture for the separate post-terminal test.
                for name in ('pre-dispatch-recovery.json','terminal.json','reconciliation.json'): (folder/name).unlink()
                closed.unlink(); (folder/'dispatch.json').write_bytes(dispatch_bytes)
                run_id='planar-t044-'+reserved['execution_id']; evidence=jobs/run_id; evidence.mkdir()
                bound=reserved['concrete_binding']; job=JobConfig.model_validate(launch.config_for(run_id,evidence,instance,'medium'))
                (evidence/'job-config.json').write_text(job.model_dump_json())
                native_dir=evidence/run_id; native_dir.mkdir(); (native_dir/'config.json').write_text(job.model_dump_json())
                verifier=native_dir/'trial'/'verifier'; verifier.mkdir(parents=True)
                (verifier/'status.json').write_text(json.dumps({'case':'planar/native-lineage-portable','contract_version':2,'status':'pass'}))
                from smoke_codex import CACHE, CONFIG
                records_data={
                    'launcher.json':{'case':'native-lineage-portable','runnable_contract':runnable.CONTRACT,'case_sha256':launch.case_inventory(instance),'candidate_sha256':reserved['candidate_inventory_sha256'],'image_id':reserved['image_id'],'verifier_image_id':reserved['verifier_image_id'],'review_sha256':schema2.h(gate_path.read_bytes())},
                    'planar-container.json':{'image_id':reserved['image_id']},'planar-image.json':{'id':reserved['image_id']},
                    'planar-install-provenance.json':{'candidate_sha256':reserved['candidate_inventory_sha256'],'installed_path':CACHE,'plugin_id':'planar@planar-smoke-market','list_contains_plugin':True,'effective_config':{'plugins':{'_default':{'enabled':False},'planar@planar-smoke-market':{'enabled':True}},'features':CONFIG['features'],'skills':CONFIG['skills']}},
                    'planar-workspace-preflight.json':{'status':'passed','check':'empty-directory-before-model','fixture':'/workspace'},'planar-cleanup.json':{'status':'passed'},'planar-cli.json':{'version':reserved['host_version']},
                    'verifier-network.json':{'network_mode':'bridge','image_id':reserved['verifier_image_id']},'verifier-compose-up.json':{'command':['up','--no-build','--pull','never']}}
                for name,content in records_data.items(): (evidence/name).write_text(json.dumps({'execution_id':run_id,**content}))
                (evidence/'native-trial-start.json').write_text(json.dumps({'trial_name':'trial','job_dir':str(native_dir),'job_id':'job','trial_id':'trial-id'}))
                for name in ('launcher-terminal.json','watchdog-terminal.json'): (evidence/name).write_text(json.dumps({'status':'completed','return_code':0}))
                (native_dir/'result.json').write_text('{}'); (native_dir/'trial'/'result.json').write_text('{}'); (verifier/'reward.txt').write_text('1')
                outcome={'status':'pass','reason':'synthetic native','stop_account':False}
                def native_reconcile(target):
                    schema2.write_new(target/'native-result.json',{'classification':outcome,'native':{'trial_name':'trial'}})
                    schema2.write_new(target/'terminal.json',{'status':'pass','reason':'synthetic native'})
                    return outcome
                with patch('harbor.models.trial.result.TrialResult.model_validate_json',return_value=SimpleNamespace(recorded_exceptions=lambda:[])), patch.object(codex_run,'reconcile',side_effect=native_reconcile) as native:
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path))
                    native.assert_not_called()
                    records_data['verifier-network.json']['network_mode']='none'
                    (evidence/'verifier-network.json').write_text(json.dumps({'execution_id':run_id,**records_data['verifier-network.json']}))
                    self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path)['status'],'pass')
                    native.assert_called_once()
                    counts=schema2_reporting.report(out,manifest['identity'])['groups'][0]
                    self.assertEqual((counts['primary_started'],counts['primary_pass'],counts['all_attempts_pass']),(1,1,1))
                    receipt_path=folder/'reconciliation.json'; original_receipt=receipt_path.read_bytes()
                    alternate=reviews/'receipt-swap.json'; alternate.write_bytes(pv.canonical({**gate,'reviewer':'other reviewer'}))
                    swapped=schema2.read_record(receipt_path); swapped['gate_path']=str(alternate)
                    swapped['sources']=schema2_execution.receipt_sources(out,reserved,alternate,'native')
                    receipt_path.write_bytes(pv.canonical(swapped))
                    self.assertEqual(schema2_reporting.report(out,manifest['identity'])['groups'][0]['primary_unknown'],1)
                    receipt_path.write_bytes(original_receipt); alternate.unlink()
                    marker_bytes=marker.read_bytes(); marker.unlink()
                    self.assertEqual(schema2_reporting.report(out,manifest['identity'])['groups'][0]['primary_unknown'],1)
                    marker.write_bytes(marker_bytes)
                    self.assertEqual(schema2_reporting.report(out,manifest['identity'])['groups'][0]['primary_pass'],1)
                    # Crash after terminal/receipt but before shared closure: a
                    # repeat recomputes in scratch and never rewrites originals.
                    closed=records/(reserved['execution_id']+'.closed.json'); closed.unlink()
                    retained={name:(folder/name).read_bytes() for name in ('terminal.json','native-result.json')}
                    (folder/'reconciliation.json').unlink()
                    self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path)['status'],'pass')
                    self.assertTrue((folder/'reconciliation.json').is_file())
                    self.assertTrue(closed.is_file())
                    self.assertEqual(retained,{name:(folder/name).read_bytes() for name in retained})
                    self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path)['status'],'pass')
                    (folder/'native-result.json').write_bytes(pv.canonical({'classification':{'status':'error'}})); closed.unlink()
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path))
                    self.assertFalse(closed.exists())
                    counts=schema2_reporting.report(out,manifest['identity'])['groups'][0]
                    self.assertEqual((counts['primary_unknown'],counts['all_attempts_unknown']),(1,1))
                # Synthetic classifications exercise reporting of every
                # reconciled outcome, without a model or native behavior claim.
                for state in ('behavioral_failure','incomplete','error'):
                    for name in ('native-result.json','terminal.json','reconciliation.json'):
                        path=folder/name
                        if path.exists(): path.unlink()
                    if closed.exists(): closed.unlink()
                    classified={'status':state,'reason':'synthetic '+state,'stop_account':False}
                    def reconcile_state(target):
                        schema2.write_new(target/'native-result.json',{'classification':classified,'native':{'trial_name':'trial'}})
                        schema2.write_new(target/'terminal.json',{'status':state,'reason':'synthetic '+state})
                        return classified
                    with patch('harbor.models.trial.result.TrialResult.model_validate_json',return_value=SimpleNamespace(recorded_exceptions=lambda:[])), patch.object(codex_run,'reconcile',side_effect=reconcile_state):
                        self.assertEqual(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path)['status'],state)
                    with patch('harbor.models.trial.result.TrialResult.model_validate_json',return_value=SimpleNamespace(recorded_exceptions=lambda:[])):
                        counts=schema2_reporting.report(out,manifest['identity'])['groups'][0]
                    self.assertEqual((counts['primary_'+state],counts['all_attempts_'+state],counts['primary_started']),(1,1,1))
                # Real Harbor model-format pre-verifier errors reach the actual
                # codex_run.reconcile; no verifier artifacts are manufactured.
                from harbor.models.trial.config import TrialConfig
                from harbor.models.trial.result import TrialResult, ExceptionInfo, AgentInfo
                from harbor.models.task.id import LocalTaskId
                from harbor.models.job.result import JobResult, JobStats
                from datetime import datetime, timedelta, timezone
                from uuid import uuid4
                job_id, trial_id=uuid4(),uuid4(); now=datetime.now(timezone.utc)
                native_record={'job_id':str(job_id),'trial_id':str(trial_id),'trial_name':'trial','job_dir':str(native_dir)}
                (evidence/'native-trial-start.json').write_text(json.dumps(native_record))
                for name in ('planar-container.json','planar-image.json','planar-install-provenance.json','planar-workspace-preflight.json','planar-cleanup.json','planar-cli.json','verifier-network.json','verifier-compose-up.json'):
                    (evidence/name).unlink()
                shutil.rmtree(verifier)
                trial_config=TrialConfig(task=job.tasks[0],agent=job.agents[0],environment=job.environment,verifier=job.verifier,job_id=job_id,trial_name='trial')
                job_result=JobResult(id=job_id,started_at=now-timedelta(seconds=1),finished_at=now+timedelta(seconds=2),n_total_trials=1,stats=JobStats(n_completed_trials=1,n_errored_trials=1))
                (native_dir/'result.json').write_text(job_result.model_dump_json())
                for exception,expected_status,expected_stop in [('ApiUsageLimitError','incomplete',True),('AgentAuthenticationError','error',True),('AgentTimeoutError','incomplete',False)]:
                    for name in ('native-result.json','terminal.json','reconciliation.json','account-stop.json'):
                        path=folder/name
                        if path.exists(): path.unlink()
                    if closed.exists(): closed.unlink()
                    trial=TrialResult(id=trial_id,task_name='synthetic',trial_name='trial',trial_uri='synthetic',task_id=LocalTaskId(path=instance),task_checksum='synthetic',config=trial_config,agent_info=AgentInfo(name='synthetic',version='1'),exception_info=ExceptionInfo(exception_type=exception,exception_message='synthetic',exception_traceback='',occurred_at=now),started_at=now,finished_at=now+timedelta(seconds=1))
                    (native_dir/'trial'/'result.json').write_text(trial.model_dump_json())
                    verifier.mkdir(exist_ok=True)
                    (verifier/'status.json').write_text(json.dumps({'case':'planar/native-lineage-portable','contract_version':2,'status':'pass'}))
                    (verifier/'reward.txt').write_text('1\n')
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path))
                    self.assertFalse((folder/'terminal.json').exists()); shutil.rmtree(verifier)
                    contradiction=evidence/'verifier-network.json'; contradiction.write_text(json.dumps({'execution_id':run_id,'network_mode':'bridge','image_id':reserved['verifier_image_id']}))
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path))
                    self.assertFalse((folder/'terminal.json').exists()); contradiction.unlink()
                    marker_pending=evidence/'recovery-required'; marker_pending.write_text('ambiguous')
                    self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path))
                    self.assertFalse((folder/'terminal.json').exists()); marker_pending.unlink()
                    outcome=schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path)
                    self.assertEqual((outcome['status'],outcome['stop_account']),(expected_status,expected_stop))
                    self.assertEqual(closed.exists(),not expected_stop)
                    summary=schema2_reporting.report(out,manifest['identity'])['groups'][0]
                    self.assertEqual(summary['primary_'+expected_status],1)
                for name in ('native-result.json','terminal.json','reconciliation.json','account-stop.json'):
                    path=folder/name
                    if path.exists(): path.unlink()
                if closed.exists(): closed.unlink()
                trial.exception_info=None; (native_dir/'trial'/'result.json').write_text(trial.model_dump_json())
                job_result.stats.n_errored_trials=0; (native_dir/'result.json').write_text(job_result.model_dump_json())
                self.assertIsNone(schema2_execution.reconcile_runnable(out,manifest['identity'],reserved['execution_id'],gate_path))
                self.assertFalse((folder/'terminal.json').exists())
            wrong=copy.deepcopy(args); wrong['config']['native_config']['normalized_job_config']['timeout_multiplier']=2.0
            with self.assertRaises(schema2.Invalid): schema2.initialize(self.base/'other',**wrong)

    def test_derivation_matches_stored_binding(self):
        binding=self.r['concrete_binding']
        actual=runnable.derive_binding(self.job,execution_id=self.r['execution_id'],effort='medium',paths=binding['path_bindings'],caps=binding['caps'],original_verifier_environment=self.original,verifier_preparation=binding['verifier_preparation'])
        self.assertEqual(actual,binding)
        policy=self.manifest['configuration']['config']['native_config']
        derived=runnable.derive_policy(self.job,execution_id=self.r['execution_id'],evidence_raw=binding['path_bindings']['evidence']['raw'],case_raw=binding['path_bindings']['case_instance']['raw'],effort='medium',roots=policy['roots'])
        self.assertEqual(derived,policy)

    def test_noauth_preparation_builds_exact_expected_image(self):
        from types import SimpleNamespace
        output=self.reviews/'prepared.json'; seen=[]
        def run(argv,**kwargs):
            seen.append((argv,kwargs.get('env')))
            if argv[1:3]==['context','inspect']: return SimpleNamespace(returncode=0,stdout='unix:///scratch.sock\n')
            if argv[1:3]==['image','inspect']:
                image=argv[-1]; image=('sha256:'+'f'*64) if image.startswith('python:') else image
                return SimpleNamespace(returncode=0,stdout=image+'\n')
            if argv[1]=='build':
                Path(argv[argv.index('--iidfile')+1]).write_text(self.r['verifier_image_id'])
                return SimpleNamespace(returncode=0,stdout=b'completed',stderr=b'')
            self.fail('unexpected Docker command')
        with patch.object(launch,'reviews_root',return_value=self.reviews), patch('shutil.which',return_value='/mock/docker'), patch('subprocess.run',side_effect=run):
            result=runnable.prepare_image(self.root,self.identity,'a:1',output=output)
        self.assertEqual(result['authorization'],'not granted')
        build=next(row for row in seen if row[0][1]=='build')
        self.assertIn('--network=none',build[0]); self.assertIn('--pull=false',build[0])
        self.assertNotIn('OPENAI_API_KEY',build[1]); self.assertNotIn('CODEX_AUTH_JSON_PATH',build[1])
        self.assertTrue(output.is_file()); self.assertTrue(output.with_name(output.name+'.build.log').is_file())
        with patch.object(launch,'reviews_root',return_value=self.reviews), self.assertRaises(schema2.Invalid):
            runnable.prepare_image(self.root,self.identity,'a:1',output=output)

    def test_review_only(self):
        self.assertEqual(self.check()['authorization'],'not granted')
        self.assertEqual(list((self.root/'executions').iterdir()),[])
    def test_old_gate_and_record_refuse(self):
        self.gate.pop('runnable_contract')
        with self.assertRaises(schema2.Invalid): self.check()
    def test_job_evidence_and_gate_drift(self):
        self.job.timeout_multiplier=2.0
        with self.assertRaises(schema2.Invalid): self.check()
        self.job.timeout_multiplier=1.0
        self.evidence.write_text('changed')
        with self.assertRaises(schema2.Invalid): self.check()
        self.gate['image_id']='sha256:'+'e'*64
        with self.assertRaises(schema2.Invalid): self.check()

if __name__=='__main__': unittest.main()
