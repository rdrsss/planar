"""Opt-in real Harbor/Docker no-model step-gate and no-fault smoke control."""
import asyncio
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from datetime import datetime,timedelta,timezone

from harbor.job import Job
from harbor.agents.nop import NopAgent
import importlib.util
from types import SimpleNamespace
from harbor.models.job.config import JobConfig
import launch
from smoke_codex import SmokeCodex,CONFIG,HOME

CASE=Path(__file__).resolve().parents[1]

class SyntheticSmoke(SmokeCodex):
    async def run(self,instruction,environment,context):
        await self._prepare_clean_step(instruction,environment)
        await self._ensure_agent_owned_dir(environment,self._REMOTE_CODEX_HOME)
        try:
            await self._upload_effective_config(environment,CONFIG,HOME+'/config.toml')
            with tempfile.TemporaryDirectory(prefix='clean-work-synthetic-') as tmp:
                root=Path(tmp); sys.path.insert(0,str(self._case))
                try:
                    from test_native import examples,rebase
                    parents,children=examples(); step=self._clean_step; index=0 if step=='worker' else 1
                    now=time.time_ns(); origin=0 if index==0 else 15
                    logs=root/'logs/sessions/2026/01/01'; logs.mkdir(parents=True)
                    for i,rows in enumerate((parents[index],children[index])):
                        (logs/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rebase(rows,now,origin)))
                    if step=='worker':
                        from test_assess_worker import example
                        data=example()
                        for name,value in data.items():
                            dest=root/'payload'/name; dest.parent.mkdir(parents=True,exist_ok=True); dest.write_bytes(value)
                        await environment.upload_dir(root/'payload/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                        for name in ('RESULT.txt','STATE.txt'): await environment.upload_file(root/'payload/repository'/name,'/workspace/repository/'+name)
                    else:
                        result=await environment.exec("test \"$(cat /workspace/repository/RESULT.txt)\" = 2 && test \"$(cat /workspace/repository/STATE.txt)\" = correct",timeout_sec=20)
                        if result.return_code: raise ValueError('worker products not preserved')
                    await environment.upload_dir(root/'logs','/logs/agent')
                finally: sys.path.remove(str(self._case))
        finally:
            await self.exec_as_root(environment,command='rm -rf /tmp/codex-secrets /installed-agent/codex-home && test ! -e /tmp/codex-secrets && test ! -e /installed-agent/codex-home',timeout_sec=30)

class SyntheticNop(NopAgent):
    """Native Nop control with synthetic A; invokes the real case-selected B hook."""
    async def run(self,instruction,environment,context):
        case=Path(os.environ['PLANAR_T044_CASE_SNAPSHOT']); evidence=Path(os.environ['PLANAR_T044_EVIDENCE_DIR'])
        from clean_revision_support import load_case_module
        identity=load_case_module(case,'step_identity'); step=identity.identify(instruction,identity.load_instructions(case))
        with tempfile.TemporaryDirectory(prefix='clean-work-nop-') as tmp:
            root=Path(tmp); sys.path.insert(0,str(case))
            try:
                from test_native import examples,rebase
                parents,children=examples(); index=0 if step=='worker' else 1; origin=0 if index==0 else 15
                if step=='worker':
                    spec=importlib.util.spec_from_file_location('clean_work_probe_fixture',case/'install_fixture.py'); fixture=importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
                    workspace=root/'workspace'; workspace.mkdir(); fixture.install(workspace)
                    await environment.upload_dir(workspace/'home','/workspace/home'); await environment.upload_dir(workspace/'repository','/workspace/repository')
                    short=await environment._platform._resolve_service_container('main'); cid=subprocess.check_output(['docker','inspect','--format','{{.Id}}',short],text=True).strip()
                    capture=load_case_module(case,'capture'); baseline=capture.census(cid); clean=evidence/'clean-revision'; clean.mkdir(mode=0o700); capture.save(clean/'initial',await capture.collect(environment,cid,baseline)); (clean/'container.json').write_text(json.dumps({'container_id':cid,'baseline':baseline})+'\n'); (clean/'active.json').write_text('{"step":"worker"}\n'); self._probe_cid=cid; self._probe_baseline=baseline
                    from test_assess_worker import example
                    data=example()
                    for name,value in data.items():
                        dest=root/'payload'/name; dest.parent.mkdir(parents=True,exist_ok=True); dest.write_bytes(value)
                    await environment.upload_dir(root/'payload/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                    for name in ('RESULT.txt','STATE.txt'): await environment.upload_file(root/'payload/repository'/name,'/workspace/repository/'+name)
                else:
                    fake=SmokeCodex.__new__(SmokeCodex); fake._case=case; fake._case_name='clean-work-review'; fake._provenance_dir=evidence; fake._clean_step='worker'; fake._clean_container=self._probe_cid; fake._clean_baseline=self._probe_baseline; fake.model_name='gpt-6.1-terra'; fake.options=SimpleNamespace(reasoning_effort='medium')
                    await fake._prepare_clean_step(instruction,environment)
                    result=await environment.exec("test \"$(cat /workspace/repository/RESULT.txt)\" = 2 && test \"$(cat /workspace/repository/STATE.txt)\" = correct",timeout_sec=20)
                    if result.return_code: raise ValueError('worker products not preserved')
                now=time.time_ns()
                logs=root/'logs/sessions/2026/01/01'; logs.mkdir(parents=True)
                for i,rows in enumerate((parents[index],children[index])): (logs/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rebase(rows,now,origin)))
                await environment.upload_dir(root/'logs','/logs/agent')
            finally: sys.path.remove(str(case))


async def main(output,candidate,agent_kind="smoke"):
    output,candidate=map(Path,(output,candidate))
    if output.exists() or output.is_symlink(): raise ValueError('output exists')
    if any(os.environ.get(k) for k in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON')): raise ValueError('credentials not allowed')
    output.mkdir(mode=0o700,parents=True); case=output/'case'; shutil.copytree(CASE,case,ignore=shutil.ignore_patterns('__pycache__'))
    evidence=output/'jobs'/('planar-t044-noauth-'+uuid.uuid4().hex); evidence.mkdir(mode=0o700,parents=True)
    expiry=(datetime.now(timezone.utc)+timedelta(hours=1)).isoformat()
    os.environ.update({'PLANAR_EVALS_OUTPUT_ROOT':str(output),'PLANAR_T044_EVIDENCE_DIR':str(evidence),'PLANAR_T044_CASE':'clean-work-review','PLANAR_T044_CASE_SNAPSHOT':str(case),'PLANAR_T044_CANDIDATE_SNAPSHOT':str(candidate),'PLANAR_T045_NOAUTH_CONTROL':'1','PLANAR_T044_ABSOLUTE_DEADLINE':expiry,'PLANAR_T044_DEADLINE_SECONDS':'1050','PLANAR_T045_VERIFIER_IMAGE_ID':'sha256:f03ecae205332adb3921400abd71c09af102b58a7843a3e5af8be926ecc5fdb6'})
    digest=launch.case_inventory(case); os.environ['PLANAR_T044_CASE_SHA256']=digest
    (evidence/'launcher.json').write_text(json.dumps({'execution_id':evidence.name,'case':'clean-work-review','case_sha256':digest,'prerequisites':{'case_read_root':str(case)},'synthetic_noauth':True})+'\n')
    (evidence/'harbor-process.json').write_text(json.dumps({'execution_id':evidence.name,'started_monotonic_ns':time.monotonic_ns(),'deadline_seconds':1050,'absolute_deadline':expiry,'synthetic_control':True})+'\n')
    cfg=JobConfig.model_validate({'job_name':evidence.name,'jobs_dir':str(evidence),'n_attempts':1,'n_concurrent_trials':1,'retry':{'max_retries':0},'agents':[({'import_path':'native_noauth_probe:SyntheticSmoke','model_name':'gpt-6.1-terra','resume_trajectory':False,'kwargs':{'version':'0.169.0','config':CONFIG,'reasoning_effort':'medium'}} if agent_kind=='smoke' else {'import_path':'native_noauth_probe:SyntheticNop','resume_trajectory':False})],'environment':{'import_path':'guarded_docker:GuardedDocker','delete':True,'kwargs':{'auto_mounting':False}},'tasks':[{'path':str(case)}],'quiet':True})
    job=await Job.create(cfg)
    async def started(event): (evidence/'native-trial-start.json').write_text(json.dumps({'trial_name':event.trial_name,'job_dir':str(job.job_dir)})+'\n')
    job.on_trial_started(started); result=await asyncio.wait_for(job.run(),timeout=240); (output/'result.json').write_text(result.model_dump_json(indent=2))
    paths=list((evidence/evidence.name).glob('*/result.json'))
    if len(paths)!=1: raise RuntimeError('missing trial result')
    trial=paths[0].parent; steps=json.loads(paths[0].read_text()).get('step_results') or []
    if len(steps)!=2 or (steps[0].get('verifier_result') or {}).get('rewards',{}).get('reward')!=1 or (steps[1].get('verifier_result') or {}).get('rewards',{}).get('reward')!=0: raise RuntimeError('unexpected native step results')
    b=json.loads((trial/'steps/review/verifier/status.json').read_text()); receipt=json.loads((evidence/'clean-revision/no-fault.json').read_text())
    if receipt.get('mode')!='none' or receipt['pre_products']!=receipt['post_products'] or b.get('status')!='behavioral_failure': raise RuntimeError('unexpected no-fault outcome')
    for project_path in (evidence/'projects').glob('*.json'):
        project=json.loads(project_path.read_text())['compose_project']; label='label=com.docker.compose.project='+project
        for args in (['container','ls','-aq'],['network','ls','-q'],['volume','ls','-q']):
            z=subprocess.run(['docker',*args,'--filter',label],capture_output=True,text=True,check=True,timeout=15)
            if z.stdout.strip(): raise RuntimeError('owned resources remain')
    proof={'mode':'native-harbor-docker-noauth-synthetic-'+agent_kind,'execution_id':evidence.name,'case_sha256':digest,'worker_reward':1,'review_reward':0,'review_status':b,'review_control':'synthetic B deliberately omits review and approval','no_fault':receipt,'resources_remaining':[]}
    (output/'proof.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n'); print(json.dumps(proof,sort_keys=True))

if __name__=='__main__':
    if len(sys.argv) not in (3,4) or (len(sys.argv)==4 and sys.argv[3]!='nop'): raise SystemExit('usage: native_noauth_probe.py NEW_OUTPUT CANDIDATE [nop]')
    asyncio.run(main(*sys.argv[1:]))
