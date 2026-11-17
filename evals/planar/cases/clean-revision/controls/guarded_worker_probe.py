"""Synthetic no-auth Harbor test of the guarded worker artifact path."""
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from datetime import datetime, timedelta, timezone
from harbor.agents.nop import NopAgent
from harbor.job import Job
from harbor.models.job.config import JobConfig
import launch
from smoke_codex import SmokeCodex, CONFIG, HOME
from clean_revision_support import load_case_module

CASE = Path(__file__).resolve().parents[1]

class SyntheticWorker(NopAgent):
    async def run(self, instruction, environment, context):
        if instruction.strip() != 'synthetic worker control':
            return await self._revision(instruction, environment, context)
        case = Path(os.environ['PLANAR_T044_CASE_SNAPSHOT'])
        capture = load_case_module(case,'capture')
        evidence = Path(os.environ['PLANAR_T044_EVIDENCE_DIR'])/'clean-revision'
        with tempfile.TemporaryDirectory(prefix='planar-guarded-worker-') as tmp:
            root = Path(tmp); workspace = root/'workspace'; workspace.mkdir()
            spec = importlib.util.spec_from_file_location('probe_fixture',case/'install_fixture.py')
            fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
            fixture.install(workspace)
            await environment.exec('mkdir -p /workspace')
            await environment.upload_dir(workspace/'home','/workspace/home')
            await environment.upload_dir(workspace/'repository','/workspace/repository')
            short = await environment._platform._resolve_service_container('main')
            cid = subprocess.check_output(['docker','inspect','--format','{{.Id}}',short],text=True).strip()
            baseline = capture.census(cid)
            self._probe_cid, self._probe_baseline = cid, baseline
            initial = await capture.collect(environment,cid,baseline)
            evidence.mkdir(mode=0o700)
            capture.save(evidence/'initial',initial)
            (evidence/'container.json').write_text(json.dumps({'container_id':cid,'baseline':baseline})+'\n')
            (evidence/'active.json').write_text('{"step":"worker"}\n')
            sys.path.insert(0,str(case))
            try:
                from test_assess_worker import example
                from test_native import examples, rebase
                data = example(); parent,child = examples(); logs = root/'logs'
                now = time.time_ns()
                for name,rows in (('parent',rebase(parent[0],now,0)),('child',rebase(child[0],now,0))):
                    path = logs/'sessions/2026/01/01'/('rollout-'+name+'.jsonl')
                    path.parent.mkdir(parents=True,exist_ok=True)
                    path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
                for name,value in data.items():
                    dest = root/'payload'/name
                    dest.parent.mkdir(parents=True,exist_ok=True); dest.write_bytes(value)
                await environment.upload_dir(root/'payload/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                for name in ('RESULT.txt','STATE.txt'):
                    await environment.upload_file(root/'payload/repository'/name,'/workspace/repository/'+name)
                await environment.upload_dir(logs,'/logs/agent')
            finally:
                sys.path.remove(str(case))
        await super().run(instruction,environment,context)


    async def _revision(self, instruction, environment, context):
        from smoke_codex import SmokeCodex
        case = Path(os.environ['PLANAR_T044_CASE_SNAPSHOT'])
        evidence = Path(os.environ['PLANAR_T044_EVIDENCE_DIR'])
        fake = SmokeCodex.__new__(SmokeCodex)
        fake._case = case; fake._provenance_dir = evidence
        fake._clean_step = 'worker'; fake._clean_container = self._probe_cid
        fake._clean_baseline = self._probe_baseline
        await fake._prepare_clean_step(instruction,environment)
        with tempfile.TemporaryDirectory(prefix='planar-guarded-revision-') as tmp:
            root = Path(tmp)/'generated'; root.mkdir()
            sys.path.insert(0,str(case))
            try:
                from test_verify_b import prepare
                from test_native import examples, rebase
                prepare(root)
                parent, children = examples()
                native_root = root/'logs/revision/sessions/2026/01/01'
                now = time.time_ns()
                for i, events in enumerate([parent[1], *children[1:]]):
                    (native_root/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rebase(events,now,15)))
                await environment.upload_dir(root/'final/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                for name in ('RESULT.txt','STATE.txt'):
                    await environment.upload_file(root/'final/repository'/name,'/workspace/repository/'+name)
                await environment.upload_dir(root/'logs/revision','/logs/agent')
            finally:
                sys.path.remove(str(case))
        await super().run(instruction,environment,context)

class SyntheticSmoke(SmokeCodex):
    """Exercise the real setup/config hooks but substitute synthetic agent work."""
    async def run(self, instruction, environment, context):
        await self._prepare_clean_step(instruction,environment)
        await self._ensure_agent_owned_dir(environment, self._REMOTE_CODEX_HOME)
        try:
            await self._upload_effective_config(environment, CONFIG, HOME+'/config.toml')
            case = self._case
            with tempfile.TemporaryDirectory(prefix='planar-real-setup-control-') as tmp:
                root = Path(tmp)
                sys.path.insert(0,str(case))
                try:
                    from test_assess_worker import example
                    from test_native import examples, rebase
                    if self._clean_step == 'worker':
                        data = example(); parents,children = examples(); now=time.time_ns()
                        logs = root/'logs/sessions/2026/01/01'; logs.mkdir(parents=True)
                        for i, rows in enumerate((parents[0],children[0])):
                            (logs/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rebase(rows,now,0)))
                        for name,value in data.items():
                            dest=root/'payload'/name; dest.parent.mkdir(parents=True,exist_ok=True); dest.write_bytes(value)
                        await environment.upload_dir(root/'payload/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                        decisions = root/'decisions.md'; decisions.write_bytes(b'# Decisions\n\n## Run 2026-11-17T00:00:00+00:00\n')
                        await environment.upload_file(decisions,'/workspace/home/projects/revision-fault/decisions.md')
                        for name in ('RESULT.txt','STATE.txt'):
                            await environment.upload_file(root/'payload/repository'/name,'/workspace/repository/'+name)
                        await environment.upload_dir(root/'logs','/logs/agent')
                    else:
                        from test_verify_b import prepare
                        generated=root/'generated'; generated.mkdir(); prepare(generated)
                        parents,children=examples(); now=time.time_ns()
                        logs=generated/'logs/revision/sessions/2026/01/01'
                        for i,rows in enumerate([parents[1],*children[1:]]):
                            (logs/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rebase(rows,now,15)))
                        await environment.upload_dir(generated/'final/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                        for name in ('RESULT.txt','STATE.txt'):
                            await environment.upload_file(generated/'final/repository'/name,'/workspace/repository/'+name)
                        await environment.upload_dir(generated/'logs/revision','/logs/agent')
                finally:
                    sys.path.remove(str(case))
        finally:
            await self.exec_as_root(environment,command='rm -rf /tmp/codex-secrets /installed-agent/codex-home && test ! -e /tmp/codex-secrets && test ! -e /installed-agent/codex-home',timeout_sec=30)

async def main():
    if len(sys.argv) not in (2,3) or (len(sys.argv)==3 and sys.argv[2] not in ('--both','--smoke')): raise ValueError('new output parent and optional --both or --smoke required')
    smoke = len(sys.argv)==3 and sys.argv[2] == '--smoke'
    both = len(sys.argv)==3
    parent=Path(sys.argv[1])
    if parent.exists() or parent.is_symlink(): raise ValueError('output parent exists')
    parent.mkdir(mode=0o700)
    jobs=parent/'jobs'; jobs.mkdir()
    evidence=jobs/('planar-t044-noauth-'+uuid.uuid4().hex); evidence.mkdir(mode=0o700)
    case=parent/'case'; shutil.copytree(CASE,case,ignore=shutil.ignore_patterns('__pycache__'))
    # Fixed local base, verified below; this synthetic image is not a live freeze.
    base = subprocess.check_output(['docker','image','inspect','--format','{{.Id}}','planar-t044-agent-preflight:latest'],text=True).strip()
    if base != 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7': raise ValueError('unexpected local control base')
    df=case/'tests/Dockerfile'; df.write_text(df.read_text().replace('python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c','planar-t044-agent-preflight:latest'))
    # The projected source fixture is copied solely as control input.
    shutil.copytree(CASE.parent/'fresh-interrupted-review/fixture-v2', parent/'fresh-interrupted-review/fixture-v2')
    (case/'task.toml').write_text('''schema_version = "1.4"\nartifacts = ["/workspace/home"]\n[task]\nname = "planar/guarded-worker-control"\ndescription = "Synthetic no-model guard control"\n[environment]\ndocker_image = "sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2"\n[agent]\ntimeout_sec = 60\n[verifier]\nenvironment_mode = "separate"\ntimeout_sec = 60\n[verifier.environment]\nnetwork_mode = "no-network"\n[[steps]]\nname = "worker"\nmin_reward = 1\n[steps.verifier]\ntimeout_sec = 60\n[steps.verifier.env]\nPLANAR_CLEAN_PHASE = "worker"\n''')
    if smoke:
        config_text=(case/'task.toml').read_text()
        config_text=config_text.replace('[verifier.environment]\nnetwork_mode', '[verifier.environment]\ndocker_image = \"sha256:0ca35a67f73d3f023852541e9de76218ab40d692c4fa5c40bb85b094eca4c800\"\nnetwork_mode')
        (case/'task.toml').write_text(config_text)
    if both:
        with (case/'task.toml').open('a') as task:
            task.write('[[steps]]\nname = "revision"\n[steps.verifier]\ntimeout_sec = 60\n[steps.verifier.env]\nPLANAR_CLEAN_PHASE = "revision"\n')
    if not smoke: (case/'steps/worker/instruction.md').write_text('synthetic worker control\n')
    for key in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON'): os.environ.pop(key,None)
    os.environ.update({'PLANAR_T044_EVIDENCE_DIR':str(evidence),'PLANAR_T044_CASE':'clean-revision','PLANAR_T044_CASE_SNAPSHOT':str(case),'PLANAR_T044_CANDIDATE_SNAPSHOT':str(CASE.parents[3]/'plugins/planar')})
    os.environ['PLANAR_EVALS_OUTPUT_ROOT']=str(parent)
    absolute = (datetime.now(timezone.utc)+timedelta(hours=1)).isoformat()
    os.environ['PLANAR_T044_ABSOLUTE_DEADLINE']=absolute
    os.environ['PLANAR_T044_DEADLINE_SECONDS']='1050'
    # Synthetic host watchdog record for this no-model direct Harbor control.
    (evidence/'harbor-process.json').write_text(json.dumps({'execution_id':evidence.name,
        'started_monotonic_ns':time.monotonic_ns(),'deadline_seconds':1050,
        'absolute_deadline':absolute,'synthetic_control':True})+'\n')
    os.environ['PLANAR_T045_NOAUTH_CONTROL']='1'
    if smoke: os.environ['PLANAR_T045_VERIFIER_IMAGE_ID']='sha256:0ca35a67f73d3f023852541e9de76218ab40d692c4fa5c40bb85b094eca4c800'
    digest=launch.case_inventory(case); os.environ['PLANAR_T044_CASE_SHA256']=digest
    (evidence/'launcher.json').write_text(json.dumps({'execution_id':evidence.name,'case':'clean-revision','case_sha256':digest,'prerequisites':{'case_read_root':str(case)}})+'\n')
    config=JobConfig.model_validate({'job_name':evidence.name,'jobs_dir':str(evidence),'n_attempts':1,'n_concurrent_trials':1,'retry':{'max_retries':0},'agents':[({'import_path':'guarded_worker_probe:SyntheticSmoke','model_name':'gpt-6.1-terra','resume_trajectory':False,'kwargs':{'version':'0.169.0','config':CONFIG,'reasoning_effort':'medium'}} if smoke else {'import_path':'guarded_worker_probe:SyntheticWorker','resume_trajectory':False})],'environment':{'import_path':'guarded_docker:GuardedDocker','delete':True,'kwargs':{'auto_mounting':False}},'tasks':[{'path':str(case)}],'quiet':True})
    (parent/'config.json').write_text(config.model_dump_json(indent=2))
    job=await Job.create(config)
    async def started(event):
        (evidence/'native-trial-start.json').write_text(json.dumps({'trial_name':event.trial_name,'job_dir':str(job.job_dir)})+'\n')
    job.on_trial_started(started)
    result=await asyncio.wait_for(job.run(),timeout=180)
    (parent/'result.json').write_text(result.model_dump_json(indent=2))
    results=list((evidence/evidence.name).glob('*/result.json'))
    if len(results)!=1: raise RuntimeError('missing trial output')
    trial=json.loads(results[0].read_text()); steps=trial.get('step_results') or []
    if len(steps)!=(2 if both else 1) or any((step.get('verifier_result') or {}).get('rewards',{}).get('reward') != 1 for step in steps):
        raise RuntimeError('guarded verifier failed; see '+str(results[0]))
    print(str(parent),flush=True)

if __name__=='__main__': asyncio.run(main())
