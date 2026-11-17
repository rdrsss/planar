"""Read-only checks for the specifically audited retained-B early stop.

This is not an account resolver. A separate independent release review and
process-lifetime evidence are required before any account publication.
"""
import hashlib
import json
import os
from pathlib import Path
import stat

from candidate import inventory
import launch
from retained_b_early_snapshots import inspect as inspect_snapshots
from retained_b_early_trace import inspect_trace
from retained_b_history import historical_inventory
from retained_b_support import source

EID='planar-t044-aeaec469c5c549ee96f3ce7729dc4c2e'
JOB='fdc75ee2-c657-453d-ab8a-0b86879b5e36'
TRIAL='89273082-831e-4cfd-812e-307db6df2413'
AUDIT_SHA='1b6f7e187c154fce391d7eccd341293fc19e00f6cfbbc949255838c9a5413578'
SOURCE_SHA='463a277722369fec5891ed689b64505d3ef15ae1030d1b8bd72b1ff61344648c'
CASE_SHA='90141823bb3ea4c18f47de66d9dba47e730dfb971bf65a615342ec009f031444'
CANDIDATE_SHA='02a5932125696d1c1cbe586f010d59c33b74a1b8b2c765c6c9b911adb6d317d3'
GATE_SHA='8a7d024b4f8ec14983f700e352deeddfeeb9209fb28a4d9545fd6336132afe86'
SNAPSHOT_SHA={'restored-pre':'5f0c625c9d861508bfd44f504e442f1083badf99bd10fb216fd36d846664b3fb','post':'16636a725be887664e74eb04053d12612c4f91c937ff6a3172fef4797eabc821','final':'16636a725be887664e74eb04053d12612c4f91c937ff6a3172fef4797eabc821'}


def read_safe(path, root, limit=4*1024*1024):
    path,root=Path(path),Path(root).resolve(strict=True)
    if not path.is_absolute() or not path.is_relative_to(root): raise ValueError('evidence path outside root')
    part=root
    for name in path.relative_to(root).parts:
        part=part/name
        if part.is_symlink(): raise ValueError('evidence path link')
    info=path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink!=1 or info.st_size>limit: raise ValueError('unsafe evidence file')
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    with os.fdopen(fd,'rb') as f: raw=f.read(limit+1)
    if len(raw)!=info.st_size: raise ValueError('evidence size changed')
    return raw


def inspect(evidence,case,candidate,packet,audit):
    reviews=(launch.PROJECT/'reviews').resolve(strict=True); scope=launch.reviews_root().resolve(strict=True); evidence=Path(evidence)
    if evidence.is_symlink() or evidence.resolve(strict=True).parent!=launch.jobs_root().resolve() or evidence.name!=EID:
        raise ValueError('unexpected execution evidence')
    raw=read_safe(audit,reviews)
    if hashlib.sha256(raw).hexdigest()!=AUDIT_SHA: raise ValueError('independent audit differs')
    document=json.loads(raw)
    if (document.get('execution_id')!=EID or document.get('job_id')!=JOB or document.get('trial_id')!=TRIAL
        or document.get('source_packet_sha256')!=SOURCE_SHA or document.get('candidate_sha256')!=CANDIDATE_SHA
        or document.get('case_sha256')!=CASE_SHA or document.get('observed_verifier_status')!={'status':'behavioral_failure','reason':'final record field mismatch'}
        or document.get('observed_reward')!='0'):
        raise ValueError('independent audit identity mismatch')
    if any(type(document.get(k)) is not bool or document[k] is not True for k in ('full_runtime_authority_revalidated','native_identity_equal','submitted_actual_jobconfigs_equal')):
        raise ValueError('audit provenance is incomplete')
    cleanup=document.get('cleanup')
    if not isinstance(cleanup,dict) or any(type(cleanup.get(k)) is not bool or cleanup[k] is not True for k in ('group_absent','recovery_marker_absent')):
        raise ValueError('audit cleanup provenance is incomplete')
    artifacts=document.get('artifacts')
    if not isinstance(artifacts,list) or len(artifacts)!=19: raise ValueError('independent audit inventory mismatch')
    paths=set()
    for entry in artifacts:
        if not isinstance(entry,dict) or set(entry)!={'path','sha256'} or entry['path'] in paths: raise ValueError('ambiguous audit item')
        paths.add(entry['path']); data=read_safe(entry['path'],reviews)
        if hashlib.sha256(data).hexdigest()!=entry['sha256']: raise ValueError('audited evidence changed')
    history=historical_inventory(launch.ROOT)
    if history['implementation_sha256']!=document.get('executed_implementation_sha256') or history['commit']!=document.get('executed_tracked_commit'):
        raise ValueError('historical implementation mismatch')
    if launch.case_inventory(Path(case))!=CASE_SHA or inventory(Path(candidate))['sha256']!=CANDIDATE_SHA:
        raise ValueError('frozen case or candidate changed')
    if source(Path(case),Path(packet),Path(candidate)).get('source_packet_sha256')!=SOURCE_SHA:
        raise ValueError('source packet changed')
    admission=json.loads(read_safe(evidence/'retained-admission.json',reviews)); gate_path=Path(admission['gate_path']); gate_bytes=read_safe(gate_path,reviews); gate=json.loads(gate_bytes)
    if hashlib.sha256(gate_bytes).hexdigest()!=GATE_SHA or admission.get('gate_authority')!=gate or admission.get('gate_raw_sha256')!=GATE_SHA:
        raise ValueError('gate authority changed')
    canonical=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    if read_safe(scope/'harbor-t044-consumed'/canonical,reviews)!=b'reserved\n': raise ValueError('gate consumption missing')
    account=scope/'planar-account-admission-v1/records'/(EID+'.start.json'); start=json.loads(read_safe(account,reviews))
    if start!={'execution_id':EID,'case':'retained-a-fresh-b','gate_sha256':GATE_SHA,'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'}:
        raise ValueError('account authority changed')
    launcher=json.loads(read_safe(evidence/'launcher.json',reviews)); config=json.loads(read_safe(evidence/'job-config.json',reviews))
    for key in ('execution_id','case','case_sha256','candidate_sha256','implementation_sha256','configuration_sha256','image_id'):
        if launcher.get(key)!=gate.get(key): raise ValueError('launcher authority changed')
    if launcher.get('review_sha256')!=GATE_SHA or hashlib.sha256(json.dumps(config,sort_keys=True,separators=(',',':')).encode()).hexdigest()!=gate.get('effective_configuration_sha256'):
        raise ValueError('job configuration changed')
    native=json.loads(read_safe(evidence/'native-trial-start.json',reviews))
    if native.get('job_id')!=JOB or native.get('trial_id')!=TRIAL: raise ValueError('native identity mismatch')
    jobs=evidence/EID; trial_paths=list(jobs.glob('*/result.json'))
    if len(trial_paths)!=1 or trial_paths[0].parent.name!=native.get('trial_name'): raise ValueError('ambiguous trial evidence')
    job=json.loads(read_safe(jobs/'result.json',reviews)); trial=json.loads(read_safe(trial_paths[0],reviews))
    if job.get('id')!=JOB or job.get('n_total_trials')!=1 or trial.get('id')!=TRIAL or trial.get('config',{}).get('job_id')!=JOB or trial.get('verifier_result',{}).get('rewards')!={'reward':0.0}:
        raise ValueError('native result mismatch')
    status=json.loads(read_safe(trial_paths[0].parent/'verifier/status.json',reviews)); reward=read_safe(trial_paths[0].parent/'verifier/reward.txt',reviews)
    if status!={'status':'behavioral_failure','reason':'final record field mismatch'} or reward.strip()!=b'0': raise ValueError('observed result changed')
    for stage,expected in SNAPSHOT_SHA.items():
        if hashlib.sha256(read_safe(evidence/'retained-b'/stage/'snapshot-manifest.json',reviews)).hexdigest()!=expected:
            raise ValueError('audited snapshot manifest changed')
    trace=inspect_trace(read_safe(evidence/'retained-b/logs/revision/codex.txt',reviews)); snapshots=inspect_snapshots(evidence/'retained-b')
    return {'execution_id':EID,'job_id':JOB,'trial_id':TRIAL,'historical_implementation':history,'trace':trace,'snapshots':snapshots,'observed_status':status,'reward':0,'account':'pending'}
