import hashlib
import json
import shutil
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch

import clean_work_evidence as checks
import launch
from clean_work_outcomes import observe
from test_clean_work_release_schema import CleanWorkReleaseSchema


def write(path,obj):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(obj,sort_keys=True)+'\n')
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


class CleanWorkEvidence(TestCase):
    def test_native_binding_and_mutated_grade(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs'; jobs.mkdir(); doc=CleanWorkReleaseSchema().document(); eid=doc['execution_id']; evidence=jobs/eid; evidence.mkdir(); scope=root/'planar-account-admission-v1'; records=scope/'records'; records.mkdir(parents=True); case=root/'case'; case.mkdir(); candidate=root/'candidate'; candidate.mkdir()
            doc.update({'job_id':'00000000-0000-4000-8000-000000000001','trial_id':'00000000-0000-4000-8000-000000000002','gate_path':str(root/'gate.json'),'authority_path':str(root/'authority.json'),'raw_manifest_path':str(root/'raw.json')})
            cfg=launch.config_for(eid,evidence,case,'medium'); config_hash=checks.digest(checks.canonical(cfg)); doc['effective_configuration_sha256']=config_hash
            gate={'verdict':'approve','purpose':'one-diagnostic-attempt','max_attempts':1,'execution_id':eid,'case':'clean-work-review','model_name':'gpt-6.1-terra','reasoning_effort':'medium','account_scope_id':str(scope),'case_sha256':doc['case_sha256'],'candidate_sha256':doc['candidate_sha256'],'configuration_sha256':doc['configuration_sha256'],'effective_configuration_sha256':config_hash,'implementation_sha256':doc['historical_implementation_sha256'],'image_id':'image','verifier_image_id':'verifier','absolute_deadline':'2100-01-01T00:00:00+00:00'}
            gate_path=Path(doc['gate_path']); write(gate_path,gate); doc['gate_sha256']=sha(gate_path); consumed=root/'harbor-t044-consumed'/checks.digest(checks.canonical(gate)); consumed.parent.mkdir(); consumed.write_bytes(b'reserved\n')
            start=records/(eid+'.start.json'); write(start,{'execution_id':eid,'case':'clean-work-review','status':'awaiting-independent-bound-raw-audit','gate_path':str(gate_path),'gate_sha256':doc['gate_sha256'],'gate_authority':gate}); doc['start_sha256']=sha(start)
            prereq={'budgets_seconds':{'job':1050,'step_agent':[400,400],'verifier':60},'case_read_root':str(case),'package_read_root':str(candidate),'effective_configuration_sha256':config_hash,'reserved_execution_id':eid,'absolute_deadline':gate['absolute_deadline']}
            write(evidence/'launcher.json',{'execution_id':eid,'case':'clean-work-review','case_sha256':doc['case_sha256'],'candidate_sha256':doc['candidate_sha256'],'configuration_sha256':doc['configuration_sha256'],'implementation_sha256':doc['historical_implementation_sha256'],'review_sha256':doc['gate_sha256'],'image_id':'image','prerequisites':prereq})
            write(evidence/'job-config.json',cfg); job=evidence/eid; job.mkdir(); write(job/'config.json',cfg); name='case__abc'; td=job/name; td.mkdir(); write(evidence/'native-trial-start.json',{'job_id':doc['job_id'],'trial_id':doc['trial_id'],'job_dir':str(job),'trial_name':name})
            proof=next(Path('/Users/mn/.planar/projects/planar/reviews/t045-clean-work-native-nop-v4-20261117/jobs').iterdir()); pn=json.loads((proof/'native-trial-start.json').read_text()); pj=Path(pn['job_dir']); native_job=json.loads((pj/'result.json').read_text()); trial=json.loads((pj/pn['trial_name']/'result.json').read_text())
            native_job['id']=doc['job_id']; next(iter(native_job['stats']['evals'].values()))['reward_stats']['reward']={'0.0':[name]}; write(job/'result.json',native_job)
            typed=checks.JobConfig.model_validate(cfg); trial.update({'id':doc['trial_id'],'trial_name':name,'task_name':'planar/clean-work-review','task_id':{'path':str(case)}})
            trial['config'].update({'job_id':doc['job_id'],'trial_name':name,'trials_dir':str(job),'agent':typed.agents[0].model_dump(mode='json'),'task':typed.tasks[0].model_dump(mode='json'),'environment':typed.environment.model_dump(mode='json'),'verifier':typed.verifier.model_dump(mode='json')})
            write(td/'result.json',trial)
            for step,status,reward in [('worker',{'status':'pass'},'1\n'),('review',{'status':'incomplete','reason':'raw audit required'},'0\n')]:
                base=td/'steps'/step/'verifier'; write(base/'status.json',status); (base/'reward.txt').write_text(reward)
            doc['steps']=observe(td,root,trial); doc['job_result_sha256']=sha(job/'result.json'); doc['trial_result_sha256']=sha(td/'result.json'); doc['exception']=None; doc['terminal_class']='incomplete'; doc['missing_artifacts']=['final host capture','worker native logs','review native logs']
            (evidence/'projects').mkdir()
            for filename in ('launcher-terminal.json','watchdog-terminal.json','harbor-process.json','worker-planar-cleanup.json','worker-planar-terminal.json','review-planar-cleanup.json','review-planar-terminal.json'): write(evidence/filename,{})
            for stage in ('worker','review'):
                write(evidence/(stage+'-planar-cleanup.json'),{'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'})
                write(evidence/(stage+'-planar-terminal.json'),{'execution_id':eid,'status':'shell_completed','return_code':0})
            required=[start,gate_path,evidence/'launcher.json',evidence/'job-config.json',evidence/'native-trial-start.json',evidence/'launcher-terminal.json',evidence/'watchdog-terminal.json',evidence/'harbor-process.json',evidence/'worker-planar-cleanup.json',evidence/'worker-planar-terminal.json',evidence/'review-planar-cleanup.json',evidence/'review-planar-terminal.json',job/'config.json',job/'result.json',td/'result.json']+[td/'steps'/s/'verifier'/n for s in ('worker','review') for n in ('status.json','reward.txt')]
            authority=Path(doc['authority_path']); write(authority,{'execution_id':eid,'job_id':doc['job_id'],'trial_id':doc['trial_id'],'full_runtime_authority_revalidated':True,'submitted_actual_jobconfigs_equal':True,'final_host_capture_exists':False,'artifacts':[{'path':str(p),'sha256':sha(p)} for p in required]}); doc['authority_sha256']=sha(authority)
            manifest=Path(doc['raw_manifest_path']); write(manifest,{'artifacts':[]}); doc['raw_manifest_sha256']=sha(manifest)
            produced=launch.Task(launch.CASES['clean-work-review']).config.verifier.timeout_sec
            self.assertIs(type(produced),float); self.assertEqual(produced,60.0)
            task=SimpleNamespace(name='planar/clean-work-review',config=SimpleNamespace(environment=SimpleNamespace(docker_image='image'),verifier=SimpleNamespace(environment=SimpleNamespace(docker_image='verifier')),multi_step_reward_strategy=SimpleNamespace(value='final'),steps=[SimpleNamespace(name='worker',min_reward=1),SimpleNamespace(name='review',min_reward=None)]))
            fake=SimpleNamespace(stdout='a'*40)
            with patch.object(launch,'reviews_root',return_value=root),patch.object(launch,'jobs_root',return_value=jobs),patch.object(launch,'case_inventory',return_value=doc['case_sha256']),patch.object(checks,'inventory',return_value={'sha256':doc['candidate_sha256']}),patch.object(checks,'historical_inventory'),patch.object(checks.subprocess,'run',return_value=fake),patch.object(launch,'Task',return_value=task):
                self.assertEqual(checks.verify(doc,evidence,start)['trial_name'],name)
                # Rebind hashes so the assertions test native identity/rewards, not only byte drift.
                original_trial=json.loads((td/'result.json').read_text())
                def attempt(mutator):
                    changed=json.loads(json.dumps(original_trial)); mutator(changed); write(td/'result.json',changed); doc['trial_result_sha256']=sha(td/'result.json')
                    audit=json.loads(authority.read_bytes()); next(x for x in audit['artifacts'] if x['path']==str(td/'result.json'))['sha256']=sha(td/'result.json'); write(authority,audit); doc['authority_sha256']=sha(authority)
                    with self.assertRaises(ValueError): checks.verify(doc,evidence,start)
                attempt(lambda t:t['config']['agent'].__setitem__('model_name','wrong'))
                attempt(lambda t:t['config']['task'].__setitem__('path','/wrong'))
                attempt(lambda t:t['task_id'].__setitem__('path','/wrong'))
                attempt(lambda t:t['config'].__setitem__('trial_name','wrong'))
                attempt(lambda t:t['step_results'][0]['verifier_result']['rewards'].__setitem__('reward',0.0))
                attempt(lambda t:t['step_results'][1]['verifier_result']['rewards'].__setitem__('reward',1.0))
                attempt(lambda t:t['verifier_result']['rewards'].__setitem__('reward',1.0))
                write(td/'result.json',original_trial); doc['trial_result_sha256']=sha(td/'result.json')
                audit=json.loads(authority.read_bytes()); next(x for x in audit['artifacts'] if x['path']==str(td/'result.json'))['sha256']=sha(td/'result.json'); write(authority,audit); doc['authority_sha256']=sha(authority)
                job_path=job/'result.json'; original_job=json.loads(job_path.read_text()); altered=json.loads(json.dumps(original_job)); next(iter(altered['stats']['evals'].values()))['reward_stats']['reward']={'1.0':[name]}; write(job_path,altered); doc['job_result_sha256']=sha(job_path)
                audit=json.loads(authority.read_bytes()); next(x for x in audit['artifacts'] if x['path']==str(job_path))['sha256']=sha(job_path); write(authority,audit); doc['authority_sha256']=sha(authority)
                with self.assertRaisesRegex(ValueError,'job grade mismatch'): checks.verify(doc,evidence,start)
                write(job_path,original_job); doc['job_result_sha256']=sha(job_path); audit=json.loads(authority.read_bytes()); next(x for x in audit['artifacts'] if x['path']==str(job_path))['sha256']=sha(job_path); write(authority,audit); doc['authority_sha256']=sha(authority)
                empty=evidence/'clean-revision'/'final'; empty.mkdir(parents=True); doc['missing_artifacts'].remove('final host capture')
                audit['final_host_capture_exists']=True; write(authority,audit); doc['authority_sha256']=sha(authority)
                with self.assertRaises((ValueError,OSError)): checks.verify(doc,evidence,start)
                empty.rmdir(); doc['missing_artifacts'].append('final host capture'); audit['final_host_capture_exists']=False; write(authority,audit); doc['authority_sha256']=sha(authority)
                for target in (gate_path,job/'config.json',job/'result.json',td/'result.json',authority,manifest):
                    before=target.read_bytes(); target.write_bytes(before+b' ')
                    with self.assertRaises((ValueError,KeyError)): checks.verify(doc,evidence,start)
                    target.write_bytes(before)
                extra=job/'other'; extra.mkdir(); write(extra/'result.json',{})
                with self.assertRaisesRegex(ValueError,'trial inventory'): checks.verify(doc,evidence,start)
                (extra/'result.json').unlink(); extra.rmdir()
                for timeout in (60,produced):
                    launcher_path=evidence/'launcher.json'; original=launcher_path.read_bytes(); launcher_data=json.loads(original); launcher_data['prerequisites']['budgets_seconds']['verifier']=timeout; write(launcher_path,launcher_data)
                    audit=json.loads(authority.read_bytes()); next(x for x in audit['artifacts'] if x['path']==str(launcher_path))['sha256']=sha(launcher_path); write(authority,audit); doc['authority_sha256']=sha(authority)
                    self.assertEqual(checks.verify(doc,evidence,start)['trial_name'],name)
                launcher_data['prerequisites']['budgets_seconds']['verifier']=True; write(launcher_path,launcher_data)
                with self.assertRaisesRegex(ValueError,'time limits'): checks.verify(doc,evidence,start)
                launcher_data['prerequisites']['budgets_seconds']['verifier']=60.0; write(launcher_path,launcher_data)
                audit=json.loads(authority.read_bytes()); next(x for x in audit['artifacts'] if x['path']==str(launcher_path))['sha256']=sha(launcher_path); write(authority,audit); doc['authority_sha256']=sha(authority)
                doc['steps']['review']['reward']=1
                with self.assertRaisesRegex(ValueError,'outcomes differ'): checks.verify(doc,evidence,start)
                doc['steps']['review']['reward']=0
                # One-step fixture derives its worker result and status from the retained run;
                # the isolated trace is redacted and its transformation is hash recorded.
                source=Path('/Users/mn/.planar/projects/planar/reviews/t045-clean-work-live-prep-20261117/output/jobs/planar-t044-891e08ce96d2474487fe2eab0ea6984b')
                source_start=json.loads((source/'native-trial-start.json').read_text()); source_trial=Path(source_start['job_dir'])/source_start['trial_name']; live_result=json.loads((source_trial/'result.json').read_text())
                one=json.loads(json.dumps(original_trial)); one['step_results']=[live_result['step_results'][0]]; one['verifier_result']=live_result['verifier_result']; write(td/'result.json',one); doc['trial_result_sha256']=sha(td/'result.json')
                shutil.rmtree(td/'steps'/'review'); status=td/'steps'/'worker'/'verifier'/'status.json'; reward=td/'steps'/'worker'/'verifier'/'reward.txt'; shutil.copyfile(source_trial/'steps/worker/verifier/status.json',status); shutil.copyfile(source_trial/'steps/worker/verifier/reward.txt',reward)
                doc['steps']=observe(td,root,one); doc['missing_artifacts']=['review verifier output','review native logs','post host capture','final host capture']
                clean=evidence/'clean-revision'; write(clean/'active.json',{'step':'worker'}); log=clean/'logs'/'worker'/'codex.txt'; log.parent.mkdir(parents=True); log.write_text('[redacted fixture: native worker events omitted]\n')
                source_log=source/'clean-revision/logs/worker/codex.txt'; transform={'source_trial_sha256':sha(source_trial/'result.json'),'source_log_sha256':sha(source_log),'transformed_trial_sha256':sha(td/'result.json'),'transformed_log_sha256':sha(log),'transform':'worker result retained; identifiers/config rebound; worker event content redacted'}; write(root/'fixture-transform.json',transform)
                write(manifest,{'artifacts':[{'path':str(log),'sha256':sha(log)}]}); doc['raw_manifest_sha256']=sha(manifest)
                raw_audit=root/'raw-audit.md'; raw_audit.write_text('Independent fixture audit of one worker incomplete/0; review and post/final absent.\n'); doc['raw_audit_path']=str(raw_audit); doc['raw_audit_sha256']=sha(raw_audit)
                for file in (evidence/'review-planar-cleanup.json',evidence/'review-planar-terminal.json'): file.unlink()
                audit=json.loads(authority.read_bytes()); audit['artifacts']=[x for x in audit['artifacts'] if '/review-planar-' not in x['path'] and '/steps/review/' not in x['path']]
                for x in audit['artifacts']:
                    path=Path(x['path']); x['sha256']=sha(path)
                audit['artifacts'] += [{'path':str(clean/'active.json'),'sha256':sha(clean/'active.json')},{'path':str(raw_audit),'sha256':sha(raw_audit)}]; write(authority,audit); doc['authority_sha256']=sha(authority)
                self.assertEqual(checks.verify(doc,evidence,start)['trial_name'],name)
                task.config.steps[0].min_reward=0
                with self.assertRaisesRegex(ValueError,'worker gate mismatch'): checks.verify(doc,evidence,start)
                task.config.steps[0].min_reward=1
                for forbidden in (clean/'no-fault.json',clean/'post',evidence/'verifier-compose-up-review.json',clean/'logs'/'review'):
                    forbidden.write_text('{}')
                    with self.assertRaisesRegex(ValueError,'contradictory review'): checks.verify(doc,evidence,start)
                    forbidden.unlink()
                modified=json.loads((td/'result.json').read_text()); modified['finished_at']=None; write(td/'result.json',modified); doc['trial_result_sha256']=sha(td/'result.json')
                with self.assertRaisesRegex(ValueError,'termination unproven'): checks.verify(doc,evidence,start)
                write(td/'result.json',one); doc['trial_result_sha256']=sha(td/'result.json')
                raw_audit.write_text('tampered')
                with self.assertRaisesRegex(ValueError,'raw audit'): checks.verify(doc,evidence,start)
                raw_audit.write_text('Independent fixture predicate audit.\n'); doc['raw_audit_sha256']=sha(raw_audit)
                source=Path('/Users/mn/.planar/projects/planar/reviews/t045-clean-clarified-live-prep-20261117/output/jobs/planar-t044-6bd4371146a84b89b323e6b70e81c85d')
                source_start=json.loads((source/'native-trial-start.json').read_text()); source_trial=Path(source_start['job_dir'])/source_start['trial_name']
                retained=json.loads((source_trial/'result.json').read_text()); one['step_results']=retained['step_results']; one['verifier_result']=retained['verifier_result']
                write(td/'result.json',one); doc['trial_result_sha256']=sha(td/'result.json')
                for filename in ('status.json','reward.txt'): shutil.copyfile(source_trial/'steps/worker/verifier'/filename,td/'steps/worker/verifier'/filename)
                write(root/'behavioral-transform.json',{'source_trial_sha256':sha(source_trial/'result.json'),'source_status_sha256':sha(source_trial/'steps/worker/verifier/status.json'),'source_reward_sha256':sha(source_trial/'steps/worker/verifier/reward.txt'),'transformed_trial_sha256':sha(td/'result.json'),'transform':'native worker result retained; identifiers and configuration rebound'})
                doc['steps']=observe(td,root,one); doc['terminal_class']='behavioral_failure'
                audit=json.loads(authority.read_bytes())
                for x in audit['artifacts']: x['sha256']=sha(Path(x['path']))
                write(authority,audit); doc['authority_sha256']=sha(authority)
                self.assertEqual(checks.verify(doc,evidence,start)['trial_name'],name)
                for wrong in ('incomplete','completed'):
                    doc['terminal_class']=wrong
                    with self.assertRaisesRegex(ValueError,'one-step outcome'): checks.verify(doc,evidence,start)
                doc['terminal_class']='behavioral_failure'
                for path in (status,reward,raw_audit,authority,gate_path,start,job/'config.json'):
                    saved=path.read_bytes(); path.write_bytes(saved+b' ')
                    with self.assertRaises((ValueError,KeyError)): checks.verify(doc,evidence,start)
                    path.write_bytes(saved)
                for forbidden in (td/'steps'/'review',clean/'no-fault.json',evidence/'verifier-network-review.json',evidence/'review-planar-cleanup.json'):
                    forbidden.write_text('{}')
                    with self.assertRaisesRegex(ValueError,'contradictory review'): checks.verify(doc,evidence,start)
                    forbidden.unlink()
                saved=status.read_bytes(); write(status,{'status':'behavioral_failure','reason':'unexpected'})
                with self.assertRaisesRegex(ValueError,'one-step worker outcome'): observe(td,root,one)
                status.write_bytes(saved)
                changed=json.loads(json.dumps(one)); changed['step_results'][0]['exception_info']={'exception_type':'error'}
                with self.assertRaisesRegex(ValueError,'one-step worker outcome'): observe(td,root,changed)
