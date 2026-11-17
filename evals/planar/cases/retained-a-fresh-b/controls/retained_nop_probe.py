"""Bounded actual Harbor/Docker no-model retained-B handoff control.

The target actors and resulting task files/native rollouts are synthetic.
This proves no model, review behavior or positive behavioral reward.
"""
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
from harbor.models.job.config import JobConfig
import launch
from smoke_codex import SmokeCodex, CONFIG, HOME
from retained_b_support import source


class SyntheticRetained(SmokeCodex):
    async def run(self,instruction,environment,context):
        if any(os.environ.get(k) for k in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON')):
            raise ValueError('noauth control cannot carry credentials')
        await self._ensure_agent_owned_dir(environment,self._REMOTE_CODEX_HOME)
        try:
            await self._upload_effective_config(environment,CONFIG,HOME+'/config.toml')
            check=await environment.exec('test ! -e /workspace/home/_planar_host && test ! -e /tmp/planar-retained-stage',timeout_sec=30)
            if check.return_code: raise ValueError('source evidence or temporary projection exposed to target')
            self._record('noauth-target-isolation.json',{'status':'passed','actors':'synthetic'})
            from clean_revision_support import check_revision_time
            check_revision_time(self._provenance_dir,475)
            sys.path.insert(0,str(self._case)); sys.path.insert(0,str(self._case/'controls'))
            try:
                from synthetic_target import main as make_synthetic
                with tempfile.TemporaryDirectory(prefix='synthetic-b-',dir=self._provenance_dir) as tmp:
                    fake=Path(tmp)/'target'
                    make_synthetic(os.environ['PLANAR_T045_SOURCE_PACKET'],self._candidate,fake)
                    await environment.upload_dir(fake/'final/home/projects/revision-fault/tasks/T-001','/workspace/home/projects/revision-fault/tasks/T-001')
                    for name in ('RESULT.txt','STATE.txt'):
                        await environment.upload_file(fake/'final/repository'/name,'/workspace/repository/'+name)
                    await environment.upload_dir(fake/'logs/revision','/logs/agent')
            finally:
                sys.path.remove(str(self._case/'controls')); sys.path.remove(str(self._case))
        finally:
            await self.exec_as_root(environment,command='rm -rf /tmp/codex-secrets /installed-agent/codex-home && test ! -e /tmp/codex-secrets && test ! -e /installed-agent/codex-home',timeout_sec=30)
            self._record('noauth-cleanup.json',{'status':'passed','paths':['/tmp/codex-secrets',HOME]})


async def main(output,packet,candidate):
    output,packet,candidate=map(Path,(output,packet,candidate))
    if output.exists() or output.is_symlink(): raise ValueError('control output exists')
    if any(os.environ.get(k) for k in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON')):
        raise ValueError('noauth control cannot carry credentials')
    output.mkdir(mode=0o700,parents=True); case=output/'case'
    shutil.copytree(Path(__file__).resolve().parents[1],case,ignore=shutil.ignore_patterns('__pycache__'))
    shutil.copytree(Path(__file__).resolve().parents[2]/'clean-revision/fixture',output/'clean-revision/fixture')
    jobs=output/'jobs'; jobs.mkdir(); evidence=jobs/('planar-t044-noauth-'+uuid.uuid4().hex); evidence.mkdir(mode=0o700)
    source_hash=source(case,packet,candidate)['source_packet_sha256']; deadline=(datetime.now(timezone.utc)+timedelta(hours=1)).isoformat()
    os.environ.update({'PLANAR_EVALS_OUTPUT_ROOT':str(output),'PLANAR_T044_EVIDENCE_DIR':str(evidence),
        'PLANAR_T044_CASE':'retained-a-fresh-b','PLANAR_T044_CASE_SNAPSHOT':str(case),
        'PLANAR_T044_CANDIDATE_SNAPSHOT':str(candidate),'PLANAR_T045_SOURCE_PACKET':str(packet),
        'PLANAR_T045_SOURCE_SHA256':source_hash,'PLANAR_T045_NOAUTH_CONTROL':'1',
        'PLANAR_T044_ABSOLUTE_DEADLINE':deadline,'PLANAR_T044_DEADLINE_SECONDS':'1050',
        'PLANAR_T045_VERIFIER_IMAGE_ID':'sha256:be62bc376e8fcc75adea278c848e4938115aef50a33ee5c92146e1719b511f73'})
    digest=launch.case_inventory(case); os.environ['PLANAR_T044_CASE_SHA256']=digest
    (evidence/'launcher.json').write_text(json.dumps({'execution_id':evidence.name,'case':'retained-a-fresh-b','case_sha256':digest,'prerequisites':{'case_read_root':str(case)},'synthetic_noauth':True})+'\n')
    (evidence/'harbor-process.json').write_text(json.dumps({'execution_id':evidence.name,'started_monotonic_ns':time.monotonic_ns(),'deadline_seconds':1050,'absolute_deadline':deadline,'synthetic_control':True})+'\n')
    config=JobConfig.model_validate({'job_name':evidence.name,'jobs_dir':str(evidence),'n_attempts':1,'n_concurrent_trials':1,'retry':{'max_retries':0},
        'agents':[{'import_path':'retained_nop_probe:SyntheticRetained','model_name':'gpt-6.1-terra','resume_trajectory':False,'kwargs':{'version':'0.169.0','config':CONFIG,'reasoning_effort':'medium'}}],
        'environment':{'import_path':'guarded_docker:GuardedDocker','delete':True,'kwargs':{'auto_mounting':False}},'tasks':[{'path':str(case)}],'quiet':True})
    job=await Job.create(config)
    async def started(event):
        (evidence/'native-trial-start.json').write_text(json.dumps({'job_id':str(event.config.job_id),'trial_id':str(event.trial_id),'trial_name':event.trial_name,'job_dir':str(job.job_dir)})+'\n')
    job.on_trial_started(started)
    result=await asyncio.wait_for(job.run(),timeout=240)
    (output/'result.json').write_text(result.model_dump_json(indent=2))
    trials=list((evidence/evidence.name).glob('*/result.json'))
    if len(trials)!=1: raise RuntimeError('missing native trial result')
    trial=trials[0].parent; status=json.loads((trial/'verifier/status.json').read_text()); reward=(trial/'verifier/reward.txt').read_text().strip()
    if status.get('status')!='incomplete' or reward!='0' or 'independent bound raw audit' not in status.get('reason',''):
        raise RuntimeError('unexpected verifier result; preserve control evidence')
    records=[json.loads(p.read_text()) for p in (evidence/'projects').glob('*.json')]; remaining=[]
    for record in records:
        label='label=com.docker.compose.project='+record['compose_project']
        for kind in (['ps','-aq'],['network','ls','-q'],['volume','ls','-q']):
            result=subprocess.run(['docker',*kind,'--filter',label],capture_output=True,text=True,check=True,timeout=15)
            if result.stdout.strip(): remaining.append({'project':record['compose_project'],'kind':kind,'ids':result.stdout.splitlines()})
    if remaining: raise RuntimeError('owned Docker resources remain')
    proof={'mode':'native-harbor-docker-noauth-synthetic','execution_id':evidence.name,'case_sha256':digest,'source_packet_sha256':source_hash,'verifier_status':status,'reward':reward,'projects':records,'resources_remaining':remaining}
    (output/'proof.json').write_text(json.dumps(proof,indent=2,sort_keys=True)+'\n')
    print(json.dumps(proof,sort_keys=True))

if __name__=='__main__':
    if len(sys.argv)!=4: raise SystemExit('usage: retained_nop_probe.py NEW_OUTPUT SOURCE_PACKET CANDIDATE')
    asyncio.run(main(*sys.argv[1:]))
