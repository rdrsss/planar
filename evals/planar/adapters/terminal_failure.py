"""Bounded account disposition for a reviewed retained-B timeout, never task acceptance."""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
from datetime import datetime,timezone

import launch
import retained_b_census
from retained_b_early_evidence import read_safe
from terminal_failure_schema import parse,unique


def h(raw): return hashlib.sha256(raw).hexdigest()
def canonical(obj): return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()


def historical_inventory(commit,tree,expected, *, case='retained-a-fresh-b'):
    if case not in ('retained-a-fresh-b','clean-work-review','clean-retained-a-continuation'):
        raise ValueError('unsupported historical case')
    paths=('evals/pyproject.toml','evals/uv.lock','evals/README.md','evals/planar/README.md','evals/planar/adapters','evals/planar/cases/'+case,'evals/planar/experiments/codex_run.py')
    actual=subprocess.run(['git','rev-parse',commit+'^{tree}'],cwd=launch.ROOT,capture_output=True,text=True,check=True,timeout=15).stdout.strip()
    if actual!=tree: raise ValueError('historical tree mismatch')
    archive=subprocess.run(['git','archive','--format=tar',commit,*paths],cwd=launch.ROOT,capture_output=True,check=True,timeout=30).stdout
    with tempfile.TemporaryDirectory(prefix='planar-timeout-history-') as tmp:
        root=Path(tmp); seen=set()
        with tarfile.open(fileobj=io.BytesIO(archive),mode='r:') as tar:
            for entry in tar.getmembers():
                path=Path(entry.name)
                if path.is_absolute() or '..' in path.parts or entry.name in seen or not (entry.isfile() or entry.isdir()): raise ValueError('unsafe historical archive')
                seen.add(entry.name); target=root/path
                if entry.isdir(): target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True); stream=tar.extractfile(entry)
                    if stream is None: raise ValueError('missing historical bytes')
                    target.write_bytes(stream.read()); target.chmod(0o400)
        files=[root/x for x in paths[:4]]+[root/paths[-1]]
        for directory in (root/paths[4],root/paths[5]): files.extend(p for p in directory.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
        records=[(str(p.relative_to(root)),h(p.read_bytes())) for p in sorted(files)]
        if h(json.dumps(records,separators=(',',':')).encode())!=expected: raise ValueError('historical inventory mismatch')


def live_check(evidence,trial_name):
    evidence=Path(evidence); eid=evidence.name
    if (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink(): raise ValueError('recovery pending')
    process=json.loads((evidence/'harbor-process.json').read_text()); pid=process.get('pid')
    if process.get('execution_id')!=eid or type(pid) is not int or pid<=1: raise ValueError('native process identity invalid')
    try: os.killpg(pid,0)
    except ProcessLookupError: pass
    except PermissionError: raise ValueError('native process state unknown') from None
    else: raise ValueError('native process group active')
    cleanup=json.loads((evidence/'planar-cleanup.json').read_text())
    if cleanup!={'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'}: raise ValueError('guest cleanup unproven')
    registry=evidence/'projects'
    if registry.is_symlink() or not registry.is_dir(): raise ValueError('registry unavailable')
    entries=list(registry.iterdir()); names=set()
    if len(entries)!=2: raise ValueError('registry ambiguous')
    for p in entries:
        if p.is_symlink() or not p.is_file() or p.suffix!='.json': raise ValueError('unsafe project record')
        obj=json.loads(p.read_text()); name=obj.get('compose_project')
        if obj.get('execution_id')!=eid or not isinstance(name,str): raise ValueError('project identity mismatch')
        names.add(name)
    prefix=(eid+'-'+trial_name).lower()
    if names!={prefix+'__env',prefix+'__verifier__trial'}: raise ValueError('project names mismatch')
    for name in names:
        for cmd in (['container','ls','-aq'],['network','ls','-q'],['volume','ls','-q']):
            r=subprocess.run(['docker',*cmd,'--filter','label=com.docker.compose.project='+name],capture_output=True,text=True,timeout=15)
            if r.returncode or r.stdout.strip(): raise ValueError('resources not absent')
    census=retained_b_census.check(launch.ROOT,eid,evidence)
    census.update({'observed_at':datetime.now(timezone.utc).isoformat(),'execution_id':eid,'native_group':'absent','project_count':2,'guest_cleanup':'recorded and container removed','historical_completion':'terminal records and independent audit'})
    return census


def validate_published(start,marker_path,marker):
    try:
        scope=Path(start).parent.parent.parent; reviews=(launch.PROJECT/'reviews').resolve(strict=True)
        expected={'schema','type','execution_id','job_id','trial_id','gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256','historical_implementation_sha256','resolver_commit','resolver_implementation_sha256','authority_sha256','status_sha256','reward_sha256','exception_sha256','review_path','review_sha256','census_path','census_sha256','observed_status','observed_reason','observed_reward'}
        if not isinstance(marker,dict) or set(marker)!=expected or marker['schema']!='planar-terminal-failure-marker-v1' or marker['type']!='terminal-failure-v1': return False
        if type(marker['observed_reward']) is not int or marker['observed_reward']!=0 or marker['observed_status']!='incomplete' or marker['observed_reason']!='unsafe packet root': return False
        review_raw=read_safe(Path(marker['review_path']),reviews); doc=parse(review_raw)
        if h(review_raw)!=marker['review_sha256']: return False
        if any(type(marker[k]) is not type(doc[k]) or marker[k]!=doc[k] for k in (set(marker)&set(doc)) - {'schema'}): return False
        shared=json.loads(Path(start).read_text(),object_pairs_hook=unique); gate=shared.get('gate_authority',{})
        if shared.get('execution_id')!=marker['execution_id'] or shared.get('gate_sha256')!=marker['gate_sha256']: return False
        for k in ('source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256'):
            if gate.get(k)!=marker[k]: return False
        if gate.get('implementation_sha256')!=marker['historical_implementation_sha256']: return False
        census_path=Path(marker['census_path'])
        if census_path.parent != Path(start).parent.resolve() or census_path.name != marker['execution_id']+'.terminal-census-'+marker['census_sha256']+'.json': return False
        raw=read_safe(census_path,reviews)
        census=json.loads(raw,object_pairs_hook=unique)
        if (h(canonical(census))!=marker['census_sha256'] or raw!=canonical(census)+b'\n'
            or type(census.get('matching_commands')) is not int or census['matching_commands']!=0
            or type(census.get('unresolved')) is not int or census['unresolved']!=0
            or census.get('execution_id')!=marker['execution_id'] or census.get('native_group')!='absent'
            or type(census.get('project_count')) is not int or census['project_count']!=2): return False
        evidence=scope/'jobs'/marker['execution_id']
        verified=verify_evidence(doc,evidence,start)
        live_check(evidence,verified['trial_name'])
        return True
    except (OSError,ValueError,TypeError,KeyError,AttributeError,subprocess.SubprocessError): return False


def verify_evidence(doc,evidence,start):
    from harbor.models.job.config import JobConfig
    from candidate import inventory
    from retained_b_support import source
    reviews=(launch.PROJECT/'reviews').resolve(strict=True); scope=launch.reviews_root().resolve(strict=True)
    evidence=Path(evidence)
    if (evidence.is_symlink() or evidence.resolve(strict=True).parent!=launch.jobs_root().resolve()
        or evidence.name!=doc['execution_id'] or not re.fullmatch(r'planar-t044-[0-9a-f]{32}',evidence.name)):
        raise ValueError('execution path mismatch')
    shared=json.loads(read_safe(start,reviews),object_pairs_hook=unique)
    if Path(start).name != doc['execution_id']+'.start.json': raise ValueError('start path mismatch')
    if shared.get('execution_id')!=doc['execution_id'] or shared.get('case')!='retained-a-fresh-b' or shared.get('status')!='awaiting-independent-bound-raw-audit': raise ValueError('start record mismatch')
    admission=json.loads(read_safe(evidence/'retained-admission.json',reviews),object_pairs_hook=unique)
    gate_path=Path(admission['gate_path']); gate_raw=read_safe(gate_path,reviews); gate=json.loads(gate_raw,object_pairs_hook=unique)
    if h(gate_raw)!=doc['gate_sha256'] or shared.get('gate_sha256')!=doc['gate_sha256'] or admission.get('gate_raw_sha256')!=doc['gate_sha256'] or shared.get('gate_authority')!=gate or admission.get('gate_authority')!=gate or gate.get('verdict')!='approve': raise ValueError('gate authority mismatch')
    marker=scope/'harbor-t044-consumed'/h(canonical(gate))
    if read_safe(marker,reviews)!=b'reserved\n': raise ValueError('consumed gate missing')
    for key in ('execution_id','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256'):
        if gate.get(key)!=doc[key]: raise ValueError('gate binding mismatch')
    if gate.get('implementation_sha256')!=doc['historical_implementation_sha256']: raise ValueError('historical gate mismatch')
    launcher=json.loads(read_safe(evidence/'launcher.json',reviews),object_pairs_hook=unique)
    for key in ('execution_id','case_sha256','candidate_sha256','configuration_sha256'):
        if launcher.get(key)!=doc[key]: raise ValueError('launcher binding mismatch')
    if launcher.get('implementation_sha256')!=doc['historical_implementation_sha256'] or launcher.get('review_sha256')!=doc['gate_sha256']: raise ValueError('launcher authority mismatch')
    if admission.get('account_scope_id')!=str(Path(start).parent.parent) or gate.get('account_scope_id')!=str(Path(start).parent.parent): raise ValueError('account scope mismatch')
    prereq=launcher['prerequisites']; case=Path(prereq['case_read_root']); candidate=Path(prereq['package_read_root']); packet=Path(prereq['source_packet_root'])
    if str(packet.resolve(strict=True))!=gate.get('source_packet_root'): raise ValueError('source path mismatch')
    if launch.case_inventory(case)!=doc['case_sha256'] or inventory(candidate)['sha256']!=doc['candidate_sha256'] or source(case,packet,candidate).get('source_packet_sha256')!=doc['source_packet_sha256']: raise ValueError('source or frozen inputs mismatch')
    historical_inventory(doc['historical_commit'],doc['historical_tree'],doc['historical_implementation_sha256'])
    job_config=read_safe(evidence/'job-config.json',reviews); parsed=json.loads(job_config,object_pairs_hook=unique)
    if h(canonical(parsed))!=doc['effective_configuration_sha256']: raise ValueError('effective config mismatch')
    native=json.loads(read_safe(evidence/'native-trial-start.json',reviews),object_pairs_hook=unique)
    if native.get('job_id')!=doc['job_id'] or native.get('trial_id')!=doc['trial_id']: raise ValueError('native identity mismatch')
    job_dir=evidence/doc['execution_id']; native_config=read_safe(job_dir/'config.json',reviews)
    if JobConfig.model_validate_json(job_config)!=JobConfig.model_validate_json(native_config): raise ValueError('typed native config mismatch')
    job=json.loads(read_safe(job_dir/'result.json',reviews)); trials=list(job_dir.glob('*/result.json'))
    if len(trials)!=1 or trials[0].parent.name!=native.get('trial_name'): raise ValueError('trial evidence ambiguous')
    trial=json.loads(read_safe(trials[0],reviews)); exc=trial.get('exception_info',{})
    if job.get('id')!=doc['job_id'] or job.get('n_total_trials')!=1 or trial.get('id')!=doc['trial_id'] or trial.get('config',{}).get('job_id')!=doc['job_id'] or exc.get('exception_type')!='AgentTimeoutError' or exc.get('exception_message')!='Agent execution timed out after 400.0 seconds': raise ValueError('timeout result mismatch')
    status=read_safe(trials[0].parent/'verifier/status.json',reviews); reward=read_safe(trials[0].parent/'verifier/reward.txt',reviews); exception=read_safe(trials[0].parent/'exception.txt',reviews)
    if json.loads(status,object_pairs_hook=unique)!={'status':'incomplete','reason':'unsafe packet root'} or reward!=b'0\n' or b'AgentTimeoutError: Agent execution timed out after 400.0 seconds' not in exception: raise ValueError('original outcome mismatch')
    if (h(status),h(reward),h(exception))!=(doc['status_sha256'],doc['reward_sha256'],doc['exception_sha256']): raise ValueError('original outcome hash mismatch')
    for name in ('launcher-terminal.json','watchdog-terminal.json'):
        terminal=json.loads(read_safe(evidence/name,reviews));
        if terminal.get('execution_id')!=doc['execution_id'] or terminal.get('status')!='completed' or type(terminal.get('return_code')) is not int or terminal['return_code']!=0: raise ValueError('supervisor terminal mismatch')
    terminal=json.loads(read_safe(evidence/'planar-terminal.json',reviews))
    if terminal.get('execution_id')!=doc['execution_id'] or terminal.get('status')!='error_or_cancelled': raise ValueError('agent terminal mismatch')
    if (evidence/'retained-b/final').exists() or (evidence/'retained-b/final').is_symlink(): raise ValueError('final capture limitation changed')
    for path_key,hash_key in (('authority_path','authority_sha256'),('parent_audit_path','parent_audit_sha256'),('child_audit_path','child_audit_sha256'),('raw_manifest_path','raw_manifest_sha256')):
        raw=read_safe(Path(doc[path_key]),reviews)
        if h(raw)!=doc[hash_key]: raise ValueError('reviewed evidence hash mismatch')
    authority=json.loads(read_safe(Path(doc['authority_path']),reviews),object_pairs_hook=unique)
    if authority.get('execution_id')!=doc['execution_id'] or authority.get('job_id')!=doc['job_id'] or authority.get('trial_id')!=doc['trial_id'] or authority.get('full_runtime_authority_revalidated') is not True or authority.get('submitted_actual_jobconfigs_equal') is not True or authority.get('final_host_capture_exists') is not False: raise ValueError('authority audit mismatch')
    artifacts=authority.get('artifacts'); seen=set()
    if not isinstance(artifacts,list) or len(artifacts)<17: raise ValueError('authority inventory incomplete')
    for item in artifacts:
        if not isinstance(item,dict) or set(item)!={'path','sha256'} or item['path'] in seen: raise ValueError('ambiguous authority artifact')
        seen.add(item['path'])
        if h(read_safe(Path(item['path']),reviews))!=item['sha256']: raise ValueError('authority artifact mismatch')
    required={str(evidence/x) for x in ('launcher.json','retained-admission.json','job-config.json','native-trial-start.json','launcher-terminal.json','watchdog-terminal.json','planar-cleanup.json','planar-terminal.json','harbor-process.json')}
    required.update((str(gate_path),str(job_dir/'config.json'),str(job_dir/'result.json'),str(trials[0]),str(trials[0].parent/'verifier/status.json'),str(trials[0].parent/'verifier/reward.txt'),str(trials[0].parent/'trial.log')))
    if not required<=seen: raise ValueError('authority inventory incomplete')
    raw_manifest=json.loads(read_safe(Path(doc['raw_manifest_path']),reviews),object_pairs_hook=unique)
    if not isinstance(raw_manifest,dict) or set(raw_manifest)!={'artifacts'} or not isinstance(raw_manifest['artifacts'],list): raise ValueError('raw manifest malformed')
    raw_seen=set(); logs=evidence/'retained-b/logs/revision'
    for item in raw_manifest['artifacts']:
        if not isinstance(item,dict) or set(item)!={'path','sha256'} or item['path'] in raw_seen: raise ValueError('raw artifact ambiguous')
        path=Path(item['path']); raw_seen.add(item['path'])
        if not path.is_relative_to(logs) or h(read_safe(path,reviews))!=item['sha256']: raise ValueError('raw artifact mismatch')
    actual={str(logs/'codex.txt')}|{str(p) for p in (logs/'sessions').rglob('rollout-*.jsonl')}
    if len(actual)<3 or raw_seen!=actual: raise ValueError('raw native inventory incomplete')
    return {'trial_name':native['trial_name'],'gate':gate}


def publish(evidence,review):
    """Explicit reviewed release; does not modify historical run or grant task acceptance."""
    import sys
    sys.path.insert(0,str(launch.EVALS/'planar/experiments'))
    try:
        import codex_run
        from retained_b_early_release import resolver_identity
        reviews=(launch.PROJECT/'reviews').resolve(strict=True); scope=launch.reviews_root().resolve(strict=True)
        evidence=Path(evidence); review=Path(review)
        if review.resolve(strict=True).is_relative_to(launch.jobs_root().resolve()): raise ValueError('review inside job evidence')
        area=scope/'planar-account-admission-v1'
        if area.is_symlink() or (area/'lock').is_symlink() or (area/'records').is_symlink(): raise ValueError('unsafe account scope')
        raw=read_safe(review,reviews); doc=parse(raw)
        if evidence.name!=doc['execution_id']: raise ValueError('execution ID mismatch')
        with codex_run.admission_scope() as records:
            start=records/(doc['execution_id']+'.start.json'); resolved=records/(doc['execution_id']+'.resolved.json'); closed=records/(doc['execution_id']+'.closed.json')
            if closed.exists() or closed.is_symlink(): raise ValueError('account already closed')
            verified=verify_evidence(doc,evidence,start)
            identity=resolver_identity()
            if identity!={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}: raise ValueError('resolver identity mismatch')
            for path in ('evals/planar/adapters/terminal_failure.py','evals/planar/adapters/terminal_failure_schema.py'):
                subprocess.run(['git','ls-files','--error-unmatch',path],cwd=launch.ROOT,capture_output=True,check=True,timeout=15)
            census=live_check(evidence,verified['trial_name'])
            census_bytes=canonical(census)+b'\n'; census_digest=h(canonical(census)); census_path=records/(doc['execution_id']+'.terminal-census-'+census_digest+'.json')
            marker={'schema':'planar-terminal-failure-marker-v1','type':'terminal-failure-v1',**{k:doc[k] for k in ('execution_id','job_id','trial_id','gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256','historical_implementation_sha256','resolver_commit','resolver_implementation_sha256','authority_sha256','status_sha256','reward_sha256','exception_sha256')},'review_path':str(review.resolve(strict=True)),'review_sha256':h(raw),'census_path':str(census_path),'census_sha256':census_digest,'observed_status':'incomplete','observed_reason':'unsafe packet root','observed_reward':0}
            if resolved.exists() or resolved.is_symlink():
                if resolved.is_symlink() or not resolved.is_file(): raise ValueError('conflicting resolution')
                previous=json.loads(resolved.read_text(),object_pairs_hook=unique)
                if not isinstance(previous,dict) or set(previous)!=set(marker) or any(type(previous[k]) is not type(marker[k]) or previous[k]!=marker[k] for k in marker if k not in ('census_path','census_sha256')) or not validate_published(start,resolved,previous): raise ValueError('conflicting resolution')
                return {'execution_id':doc['execution_id'],'account':'resolved','behavior':'incomplete'}
            fd=os.open(census_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            with os.fdopen(fd,'wb') as f: f.write(census_bytes); f.flush(); os.fsync(f.fileno())
            directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(directory)
            finally: os.close(directory)
            data=canonical(marker)+b'\n'; fd,tmp=tempfile.mkstemp(prefix='.terminal-failure-',dir=records)
            try:
                with os.fdopen(fd,'wb') as f: f.write(data); f.flush(); os.fsync(f.fileno())
                os.link(tmp,resolved,follow_symlinks=False)
                directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(directory)
                finally: os.close(directory)
            finally: os.unlink(tmp)
            return {'execution_id':doc['execution_id'],'account':'resolved','behavior':'incomplete'}
    finally:
        sys.path.remove(str(launch.EVALS/'planar/experiments'))
