"""One no-model native Harbor handoff; synthetic actor, no behavioral claim."""
import asyncio
from datetime import datetime,timedelta,timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
from harbor.job import Job
from harbor.models.job.config import JobConfig
import launch
from smoke_codex import SmokeCodex, CONFIG, HOME
from retained_b_support import source


class SyntheticClean(SmokeCodex):
    async def run(self,instruction,environment,context):
        if any(os.environ.get(k) for k in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON')):
            raise ValueError('noauth control cannot carry credentials')
        await self._ensure_agent_owned_dir(environment,self._REMOTE_CODEX_HOME)
        try:
            await self._upload_effective_config(environment,CONFIG,HOME+'/config.toml')
            check=await environment.exec('test ! -e /workspace/home/_planar_host && test ! -e /tmp/planar-retained-clean-stage',timeout_sec=30)
            if check.return_code: raise ValueError('host source evidence exposed to target')
            self._record('noauth-target-isolation.json',{'status':'passed','actors':'synthetic'})
            # Minimal synthetic logs exercise artifact export; they do not claim native reviewer work.
            command="mkdir -p /logs/agent && printf '%s\\n' '{\"type\":\"synthetic-control\"}' > /logs/agent/codex.txt"
            result=await environment.exec(command,timeout_sec=30)
            if result.return_code: raise ValueError('synthetic log write failed')
        finally:
            await self.exec_as_root(environment,command='rm -rf /tmp/codex-secrets /installed-agent/codex-home && test ! -e /tmp/codex-secrets && test ! -e /installed-agent/codex-home',timeout_sec=30)
            self._record('noauth-cleanup.json',{'status':'passed','paths':['/tmp/codex-secrets',HOME]})


async def main(output,packet,candidate):
    output,packet,candidate=map(Path,(output,packet,candidate))
    if output.exists() or output.is_symlink(): raise ValueError('control output exists')
    if any(os.environ.get(k) for k in ('OPENAI_API_KEY','CODEX_API_KEY','CODEX_AUTH_JSON_PATH','CODEX_FORCE_AUTH_JSON')): raise ValueError('credentials present')
    output.mkdir(mode=0o700,parents=True); case=output/'case'
    shutil.copytree(Path(__file__).resolve().parents[1],case,ignore=shutil.ignore_patterns('__pycache__'))
    jobs=output/'jobs'; jobs.mkdir(); evidence=jobs/('planar-t044-'+uuid.uuid4().hex); evidence.mkdir(mode=0o700)
    source_hash=source(case,packet,candidate)['source_packet_sha256']; deadline=(datetime.now(timezone.utc)+timedelta(hours=1)).isoformat()
    image=json.loads((case/'source-policy.json').read_text())['assessor_image']
    verifier='sha256:dfb35074f960ff34dfa1c1181079ad034df08ae2c8adc04871824ce0d249eb82'
    os.environ.update({'PLANAR_EVALS_OUTPUT_ROOT':str(output),'PLANAR_T044_EVIDENCE_DIR':str(evidence),'PLANAR_T044_CASE':'clean-retained-a-continuation',
        'PLANAR_T044_CASE_SNAPSHOT':str(case),'PLANAR_T044_CANDIDATE_SNAPSHOT':str(candidate),'PLANAR_T045_SOURCE_PACKET':str(packet),
        'PLANAR_T045_SOURCE_SHA256':source_hash,'PLANAR_T045_NOAUTH_CONTROL':'1','PLANAR_T044_ABSOLUTE_DEADLINE':deadline,
        'PLANAR_T044_DEADLINE_SECONDS':'1050','PLANAR_T045_VERIFIER_IMAGE_ID':verifier})
    digest=launch.case_inventory(case); os.environ['PLANAR_T044_CASE_SHA256']=digest
    (evidence/'launcher.json').write_text(json.dumps({'execution_id':evidence.name,'case':'clean-retained-a-continuation','case_sha256':digest,'prerequisites':{'case_read_root':str(case)},'synthetic_noauth':True})+'\n')
    (evidence/'harbor-process.json').write_text(json.dumps({'execution_id':evidence.name,'started_monotonic_ns':time.monotonic_ns(),'deadline_seconds':1050,'absolute_deadline':deadline,'synthetic_control':True})+'\n')
    config=JobConfig.model_validate({'job_name':evidence.name,'jobs_dir':str(evidence),'n_attempts':1,'n_concurrent_trials':1,'retry':{'max_retries':0},
        'agents':[{'import_path':'native_noauth_probe:SyntheticClean','model_name':'gpt-6.1-terra','resume_trajectory':False,'kwargs':{'version':'0.169.0','config':CONFIG,'reasoning_effort':'medium'}}],
        'environment':{'import_path':'guarded_docker:GuardedDocker','delete':True,'kwargs':{'auto_mounting':False}},'tasks':[{'path':str(case)}],'quiet':True})
    job=await Job.create(config)
    async def started(event):
        (evidence/'native-trial-start.json').write_text(json.dumps({'job_id':str(event.config.job_id),'trial_id':str(event.trial_id),'trial_name':event.trial_name,'job_dir':str(job.job_dir)})+'\n')
    job.on_trial_started(started)
    result=await asyncio.wait_for(job.run(),timeout=240); (output/'result.json').write_text(result.model_dump_json(indent=2))
    trials=list((evidence/evidence.name).glob('*/result.json'))
    if len(trials)!=1: raise RuntimeError('missing native trial result')
    trial=trials[0].parent; status=json.loads((trial/'verifier/status.json').read_text()); reward=(trial/'verifier/reward.txt').read_text().strip()
    if status.get('status')!='incomplete' or reward!='0': raise RuntimeError('unexpected verifier result')
    records=[json.loads(p.read_text()) for p in (evidence/'projects').glob('*.json')]; remaining=[]
    for record in records:
        label='label=com.docker.compose.project='+record['compose_project']
        for kind in (['ps','-aq'],['network','ls','-q'],['volume','ls','-q']):
            r=subprocess.run(['docker',*kind,'--filter',label],capture_output=True,text=True,check=True,timeout=15)
            if r.stdout.strip(): remaining.append({'project':record['compose_project'],'kind':kind,'ids':r.stdout.splitlines()})
    if remaining: raise RuntimeError('owned Docker resources remain')
    proof={'mode':'native-harbor-docker-noauth-synthetic','execution_id':evidence.name,'case_sha256':digest,'source_packet_sha256':source_hash,'verifier_status':status,'reward':reward,'projects':records,'resources_remaining':remaining}
    (output/'proof.json').write_text(json.dumps(proof,indent=2,sort_keys=True)+'\n'); print(json.dumps(proof,sort_keys=True))

if __name__=='__main__':
    if len(sys.argv)!=4: raise SystemExit('usage: native_noauth_probe.py NEW_OUTPUT SOURCE_PACKET CANDIDATE')
    asyncio.run(main(*sys.argv[1:]))
