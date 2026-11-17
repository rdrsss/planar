"""Closed account-only resolution for a completed retained-clean target."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from datetime import datetime

import launch
from harbor.models.job.config import JobConfig
from harbor.models.job.result import JobResult
from harbor.models.trial.result import TrialResult
from candidate import inventory
from clean_revision_support import load_case_module
from retained_b_early_evidence import read_safe, safe_path
from terminal_failure import historical_inventory
from terminal_failure_schema import unique


def canonical(value): return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
def digest(raw): return hashlib.sha256(raw).hexdigest()

FIELDS={'schema','decision','execution_id','job_id','trial_id','source_job_id','gate_path','gate_sha256','start_sha256',
        'case_sha256','candidate_sha256','implementation_sha256','configuration_sha256','effective_configuration_sha256',
        'source_packet_sha256','historical_commit','historical_tree','resolver_commit','resolver_implementation_sha256',
        'authority_path','authority_sha256','raw_manifest_path','raw_manifest_sha256','raw_audit_path','raw_audit_sha256',
        'job_result_sha256','trial_result_sha256','status_sha256','reward_sha256','target_status','target_reason','target_reason_present',
        'source_status','source_reward','target_reward','reviewer','independence_basis','reviewed_at','independent_review'}


def parse(raw):
    doc=json.loads(raw,object_pairs_hook=unique)
    if not isinstance(doc,dict) or set(doc)!=FIELDS or doc['schema']!='planar-retained-clean-account-v1' or doc['decision']!='release-account' or doc['independent_review'] is not True:
        raise ValueError('unsupported retained clean disposition')
    if not re.fullmatch(r'planar-t044-[0-9a-f]{32}',doc.get('execution_id','')): raise ValueError('invalid execution identity')
    if doc['source_status']!='behavioral_failure' or type(doc['source_reward']) is not int or doc['source_reward']!=0 or type(doc['target_reward']) is not int or doc['target_reward']!=0 or doc['target_status'] not in ('incomplete','behavioral_failure'):
        raise ValueError('unsupported source or target outcome')
    if type(doc['target_reason_present']) is not bool or (not doc['target_reason_present'] and doc['target_reason'] is not None): raise ValueError('invalid reason presence')
    if doc['target_reason'] is not None and (type(doc['target_reason']) is not str or not doc['target_reason']): raise ValueError('invalid target reason')
    for name in FIELDS:
        if name.endswith('_sha256') and (type(doc[name]) is not str or not re.fullmatch(r'[0-9a-f]{64}',doc[name])): raise ValueError('invalid digest')
    for name in ('historical_commit','historical_tree','resolver_commit'):
        if type(doc[name]) is not str or not re.fullmatch(r'[0-9a-f]{40}',doc[name]): raise ValueError('invalid commit')
    paths=[doc[name] for name in ('gate_path','authority_path','raw_manifest_path','raw_audit_path')]
    if len(set(paths))!=len(paths) or any(type(p) is not str or not Path(p).is_absolute() or '..' in Path(p).parts for p in paths): raise ValueError('unsafe evidence path')
    if any(type(doc[name]) is not str or not doc[name].strip() for name in ('job_id','trial_id','source_job_id','reviewer','independence_basis','reviewed_at')): raise ValueError('missing identity')
    if doc['source_job_id']==doc['job_id']: raise ValueError('source/target overlap')
    try:
        if datetime.fromisoformat(doc['reviewed_at']).tzinfo is None: raise ValueError()
    except (TypeError,ValueError): raise ValueError('invalid review time') from None
    return doc


def _time(value):
    if type(value) is not str: raise ValueError('missing native time')
    try: result=datetime.fromisoformat(value.replace('Z','+00:00'))
    except ValueError: raise ValueError('invalid native time') from None
    if result.tzinfo is None: raise ValueError('unbound native time')
    return result


def validate_shape(job,trial,config,actual,job_id,trial_id,trial_name,job_dir):
    """Match the actual one-trial, unstepped Harbor producer shape."""
    j=JobResult.model_validate(job); t=TrialResult.model_validate(trial); c=JobConfig.model_validate(config)
    if (str(j.id)!=job_id or str(t.id)!=trial_id or str(t.config.job_id)!=job_id or t.trial_name!=trial_name or
        t.config.trial_name!=trial_name or str(t.config.trials_dir)!=str(job_dir) or len(c.agents)!=1 or len(c.tasks)!=1 or
        t.config.agent!=c.agents[0] or t.config.task!=c.tasks[0] or t.config.environment!=c.environment or t.config.verifier!=c.verifier or
        t.task_name!='planar/clean-retained-a-continuation' or getattr(t.task_id,'path',None)!=c.tasks[0].path):
        raise ValueError('native configuration mismatch')
    if j.trial_results and (len(j.trial_results)!=1 or j.trial_results[0]!=t): raise ValueError('job trial mismatch')
    stats=job.get('stats',{})
    if type(job.get('n_total_trials')) is not int or job['n_total_trials']!=1 or any(type(stats.get(k)) is not int or stats[k]!=v for k,v in (('n_completed_trials',1),('n_errored_trials',0),('n_running_trials',0),('n_pending_trials',0),('n_cancelled_trials',0),('n_retries',0))): raise ValueError('job not completed exactly once')
    if trial.get('step_results','missing') is not None or trial.get('exception_info','missing') is not None or trial.get('verifier_environment_mode')!='separate' or trial.get('reward_valid') is not True:
        raise ValueError('unsupported unstepped native shape')
    times=[]
    for key in ('agent_execution','verifier'):
        item=trial.get(key)
        if not isinstance(item,dict): raise ValueError('native timing missing')
        begin,end=_time(item.get('started_at')),_time(item.get('finished_at'))
        if end < begin: raise ValueError('native timing reversed')
        times.append((begin,end))
    if times[0][1]>times[1][0] or _time(trial['started_at'])>times[0][0] or _time(trial['finished_at'])<times[1][1]: raise ValueError('native chronology mismatch')
    result=trial.get('verifier_result'); rewards=result.get('rewards') if isinstance(result,dict) else None
    if not isinstance(rewards,dict) or set(rewards)!={'reward'} or type(rewards['reward']) not in (int,float) or rewards['reward']!=0 or actual!=0: raise ValueError('native reward mismatch')
    evals=stats.get('evals')
    if not isinstance(evals,dict) or len(evals)!=1: raise ValueError('job grade unavailable')
    item=next(iter(evals.values())); scores=item.get('reward_stats',{}).get('reward',{})
    if type(item.get('n_trials')) is not int or item['n_trials']!=1 or len(scores)!=1: raise ValueError('job grade mismatch')
    k,names=next(iter(scores.items()))
    try: number=float(k)
    except (TypeError,ValueError): raise ValueError('job reward malformed') from None
    if number!=0 or names!=[trial_name]: raise ValueError('job reward mismatch')


def verify(doc,evidence,start):
    """Revalidate the exact immutable source, host and native authority."""
    evidence,start=Path(evidence),Path(start); reviews=launch.PROJECT/'reviews'; target_root=launch.reviews_root().resolve(strict=True)
    if evidence.is_symlink() or evidence.resolve(strict=True).parent!=launch.jobs_root().resolve(strict=True) or evidence.name!=doc['execution_id']:
        raise ValueError('target evidence path mismatch')
    def raw(p): return read_safe(Path(p),reviews.resolve(strict=True))
    def obj(p): return json.loads(raw(p),object_pairs_hook=unique)
    eid=doc['execution_id']; gate_path=safe_path(doc['gate_path'],target_root); gate_raw=raw(gate_path); gate=json.loads(gate_raw,object_pairs_hook=unique)
    start_raw=raw(start); shared=json.loads(start_raw,object_pairs_hook=unique)
    if start.name!=eid+'.start.json' or digest(start_raw)!=doc['start_sha256'] or digest(gate_raw)!=doc['gate_sha256']:
        raise ValueError('gate or start bytes changed')
    expected_start={'execution_id':eid,'case':'clean-retained-a-continuation','gate_sha256':doc['gate_sha256'],
                    'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'}
    if shared!=expected_start or gate.get('case')!='clean-retained-a-continuation' or gate.get('execution_id')!=eid or gate.get('verdict')!='approve' or gate.get('purpose')!='one-diagnostic-attempt' or type(gate.get('max_attempts')) is not int or gate['max_attempts']!=1:
        raise ValueError('gate authority mismatch')
    for key in ('case_sha256','candidate_sha256','implementation_sha256','configuration_sha256','effective_configuration_sha256','source_packet_sha256'):
        if gate.get(key)!=doc[key]: raise ValueError('gate binding mismatch')
    if (gate.get('worker_role_delivery')!='unverified' or gate.get('source_semantics')!='packet-read-causal-write-completion-only' or
        gate.get('source_online_status')!='behavioral_failure' or type(gate.get('source_online_reward')) is not int or gate['source_online_reward']!=0 or
        gate.get('handoff')!='no-fault' or gate.get('account_scope_id')!=str(start.parent.parent)):
        raise ValueError('source or account gate mismatch')
    consumed=target_root/'harbor-t044-consumed'/digest(canonical(gate))
    if raw(consumed)!=b'reserved\n': raise ValueError('gate consumption missing')
    launcher=obj(evidence/'launcher.json'); pre=launcher['prerequisites']
    for name in ('execution_id','case_sha256','candidate_sha256','implementation_sha256','configuration_sha256'):
        if launcher.get(name)!=doc[name]: raise ValueError('launcher binding mismatch')
    if launcher.get('case')!='clean-retained-a-continuation' or launcher.get('review_sha256')!=doc['gate_sha256'] or pre.get('reserved_execution_id')!=eid:
        raise ValueError('launcher authority mismatch')
    budget=pre.get('budgets_seconds',{})
    if budget.get('job')!=1050 or budget.get('agent')!=400 or budget.get('verifier')!=60 or budget.get('grace')!=15: raise ValueError('runtime budget mismatch')
    case=Path(pre['case_read_root']); candidate=Path(pre['package_read_root']); packet=Path(pre['source_packet_root'])
    if (launch.case_inventory(case)!=doc['case_sha256'] or inventory(candidate)['sha256']!=doc['candidate_sha256'] or
        str(packet)!=gate.get('source_packet_root') or pre.get('source_packet_sha256')!=doc['source_packet_sha256']):
        raise ValueError('frozen source mismatch')
    source=load_case_module(case,'source_packet'); binding=source.validate(packet,candidate)
    if binding['source_packet_sha256']!=doc['source_packet_sha256'] or binding['worker_role_delivery']!='unverified': raise ValueError('source packet mismatch')
    policy=obj(case/'source-policy.json')
    if policy['job']!=doc['source_job_id'] or gate.get('source_execution_id')!=policy['execution'] or gate.get('source_assessor_image')!=policy['assessor_image'] or gate.get('source_semantic_audit_sha256')!=policy['files']['evidence/semantic-audit.md']['sha256'] or gate.get('source_assessment_sha256')!=policy['files']['evidence/offline-regrade.json']['sha256']:
        raise ValueError('source identity mismatch')
    source_gate=obj(packet/'evidence/original-gate.json')
    expected_caps={'runtime_budget_seconds':1050,'step_agent_seconds':400,'step_verifier_seconds':60,'cleanup_grace_seconds':15,
                   'model_name':'gpt-6.1-terra','reasoning_effort':'medium','result_boundary':'incomplete-pending-independent-bound-raw-audit'}
    if any(type(gate.get(k)) is not type(v) or gate.get(k)!=v for k,v in expected_caps.items()): raise ValueError('reviewed cap mismatch')
    if gate.get('source_account_scope_id')!=source_gate.get('account_scope_id') or gate.get('source_account_scope_id')==gate.get('account_scope_id'):
        raise ValueError('source account scope mismatch or lock collision')
    admission=obj(evidence/'retained-admission.json')
    expected_admission={'execution_id':eid,'gate_path':str(gate_path),'gate_raw_sha256':doc['gate_sha256'],'gate_authority':gate,
        'source_packet_sha256':doc['source_packet_sha256'],'source_packet_root':str(packet),'account_scope_id':str(start.parent.parent),
        'case_sha256':doc['case_sha256'],'candidate_sha256':doc['candidate_sha256'],
        'effective_configuration_sha256':doc['effective_configuration_sha256'],'verifier_image_id':gate['verifier_image_id']}
    if admission!=expected_admission: raise ValueError('runtime admission mismatch')
    load_case_module(case,'source_account').check(packet,candidate,reviews)
    historical_inventory(doc['historical_commit'],doc['historical_tree'],doc['implementation_sha256'],case='clean-retained-a-continuation')
    resolver_tree=subprocess.run(['git','rev-parse',doc['resolver_commit']+'^{tree}'],cwd=launch.ROOT,capture_output=True,text=True,check=True,timeout=15).stdout.strip()
    historical_inventory(doc['resolver_commit'],resolver_tree,doc['resolver_implementation_sha256'],case='clean-retained-a-continuation')
    config_raw=raw(evidence/'job-config.json'); config=json.loads(config_raw,object_pairs_hook=unique)
    if digest(canonical(config))!=doc['effective_configuration_sha256'] or pre.get('effective_configuration_sha256')!=doc['effective_configuration_sha256']:
        raise ValueError('effective config mismatch')
    if len(config.get('agents',[]))!=1 or config['agents'][0].get('import_path')!='smoke_codex:SmokeCodex' or config['agents'][0].get('model_name')!='gpt-6.1-terra' or config['agents'][0].get('kwargs',{}).get('reasoning_effort')!='medium' or config['agents'][0].get('resume_trajectory') is not False or config['agents'][0].get('load_trajectory') is not None:
        raise ValueError('target agent selection mismatch')
    task=launch.Task(case).config
    from harbor.models.task.config import NetworkMode
    from harbor.models.task.verifier_mode import VerifierEnvironmentMode
    if (task.steps or task.agent.timeout_sec!=400 or task.verifier.timeout_sec!=60 or
        task.verifier.environment_mode!=VerifierEnvironmentMode.SEPARATE or task.verifier.environment is None or
        task.verifier.environment.network_mode!=NetworkMode.NO_NETWORK or task.artifacts!=['/workspace/home']):
        raise ValueError('target task contract mismatch')
    if (gate.get('image_id')!=task.environment.docker_image or launcher.get('image_id')!=gate['image_id'] or task.verifier.environment is None or gate.get('verifier_image_id')!=task.verifier.environment.docker_image): raise ValueError('image mismatch')
    native=obj(evidence/'native-trial-start.json'); job_dir=evidence/eid; name=native.get('trial_name')
    if native.get('job_id')!=doc['job_id'] or native.get('trial_id')!=doc['trial_id'] or native.get('job_dir')!=str(job_dir) or not isinstance(name,str) or not re.fullmatch(r'[A-Za-z0-9_-]+',name): raise ValueError('native start mismatch')
    native_config=raw(job_dir/'config.json')
    if JobConfig.model_validate_json(config_raw)!=JobConfig.model_validate_json(native_config): raise ValueError('native config mismatch')
    results=list(job_dir.glob('*/result.json'))
    if len(results)!=1 or results[0].parent.name!=name: raise ValueError('trial inventory ambiguous')
    jr=raw(job_dir/'result.json'); tr=raw(results[0]); job=json.loads(jr,object_pairs_hook=unique); trial=json.loads(tr,object_pairs_hook=unique)
    if digest(jr)!=doc['job_result_sha256'] or digest(tr)!=doc['trial_result_sha256']: raise ValueError('native result drift')
    status_path=results[0].parent/'verifier/status.json'; reward_path=results[0].parent/'verifier/reward.txt'
    status_raw,reward_raw=raw(status_path),raw(reward_path); status=json.loads(status_raw,object_pairs_hook=unique)
    if (digest(status_raw)!=doc['status_sha256'] or digest(reward_raw)!=doc['reward_sha256'] or reward_raw!=b'0\n' or not isinstance(status,dict) or
        status.get('status')!=doc['target_status'] or status.get('reason')!=doc['target_reason'] or ('reason' in status)!=doc['target_reason_present']): raise ValueError('verifier pair mismatch')
    validate_shape(job,trial,config,doc['target_reward'],doc['job_id'],doc['trial_id'],name,job_dir)
    target=evidence/'retained-clean'; capture=load_case_module(case,'capture'); chronology=load_case_module(case,'chronology'); boundary=load_case_module(case,'no_fault')
    restored=capture.load(target/'restored-pre'); post=capture.load(target/'post'); final=capture.load(target/'final')
    source_capture=capture.load(packet/'source/pre'); source_receipt=chronology.receipt(packet/'source','pre')
    pre_receipt=chronology.receipt(target,'restored-pre'); post_receipt=chronology.receipt(target,'post'); final_receipt=chronology.receipt(target,'final'); chronology.order(post_receipt,final_receipt)
    nofault=obj(target/'no-fault.json'); provenance=obj(target/'target-provenance.json')
    if (set(nofault)!={'source_packet_sha256','source_job','target_job','boundary','wall_ns','monotonic_ns'} or
        nofault['source_packet_sha256']!=doc['source_packet_sha256'] or nofault['source_job']!=doc['source_job_id'] or nofault['target_job']!=doc['job_id'] or nofault['boundary']!='restored-no-fault' or
        provenance!={'execution_id':eid,'job_id':doc['job_id']}): raise ValueError('host handoff identity mismatch')
    boundary.verify(source_capture,restored,post,source_receipt=source_receipt,restored_receipt=pre_receipt,post_receipt=post_receipt,
                    source_job=doc['source_job_id'],target_job=doc['job_id'],injection_exists=(target/'injection.json').exists() or (target/'injection.json').is_symlink())
    if type(nofault['wall_ns']) is not int or type(nofault['monotonic_ns']) is not int or not post_receipt['wall_ns']<=nofault['wall_ns']<final_receipt['wall_ns'] or not post_receipt['monotonic_ns']<=nofault['monotonic_ns']<final_receipt['monotonic_ns']: raise ValueError('host handoff chronology mismatch')
    if not final: raise ValueError('final capture unavailable')
    exported=results[0].parent/'artifacts/workspace/home/_planar_host'
    if exported.is_symlink() or not exported.is_dir(): raise ValueError('verifier handoff unavailable')
    if inventory(exported/'candidate')['sha256']!=doc['candidate_sha256'] or source.validate_closed(exported/'source-packet',doc['candidate_sha256'])['source_packet_sha256']!=doc['source_packet_sha256']:
        raise ValueError('verifier handoff source mismatch')
    target_copy=exported/'target'
    def regular_files(root):
        if root.is_symlink() or not root.is_dir(): raise ValueError('unsafe evidence tree')
        result={}
        for p in root.rglob('*'):
            if p.is_symlink() or not (p.is_file() or p.is_dir()): raise ValueError('unsafe evidence entry')
            if p.is_file() and p.name not in ('rejected-home.tar','rejected-home.json'): result[p.relative_to(root).as_posix()]=raw(p)
        return result
    if regular_files(target)!=regular_files(target_copy): raise ValueError('verifier handoff target drift')
    authority_raw=raw(doc['authority_path']); manifest_raw=raw(doc['raw_manifest_path']); audit=raw(doc['raw_audit_path'])
    if digest(authority_raw)!=doc['authority_sha256'] or digest(manifest_raw)!=doc['raw_manifest_sha256'] or digest(audit)!=doc['raw_audit_sha256']: raise ValueError('independent evidence drift')
    authority=json.loads(authority_raw,object_pairs_hook=unique); manifest=json.loads(manifest_raw,object_pairs_hook=unique)
    if authority.get('execution_id')!=eid or authority.get('job_id')!=doc['job_id'] or authority.get('trial_id')!=doc['trial_id'] or authority.get('full_runtime_authority_revalidated') is not True or authority.get('submitted_actual_jobconfigs_equal') is not True: raise ValueError('independent authority mismatch')
    def inventory_entries(items):
        if not isinstance(items,list): raise ValueError('evidence inventory missing')
        found=set()
        for item in items:
            if not isinstance(item,dict) or set(item)!={'path','sha256'} or type(item['path']) is not str or item['path'] in found or digest(raw(item['path']))!=item['sha256']: raise ValueError('evidence inventory mismatch')
            found.add(item['path'])
        return found
    audited=inventory_entries(authority.get('artifacts'))
    required={str(p) for p in (start,gate_path,evidence/'launcher.json',evidence/'job-config.json',evidence/'native-trial-start.json',evidence/'retained-admission.json',evidence/'launcher-terminal.json',evidence/'watchdog-terminal.json',evidence/'harbor-process.json',evidence/'planar-terminal.json',evidence/'planar-cleanup.json',evidence/'verifier-compose-up.json',evidence/'verifier-network.json',job_dir/'config.json',job_dir/'result.json',results[0],status_path,reward_path,target/'no-fault.json',target/'target-provenance.json',target/'container.json',Path(doc['raw_audit_path']))}
    for stage in ('restored-pre','post','final'):
        required.add(str(target/(stage+'-receipt.json'))); required.add(str(target/stage/'snapshot-manifest.json'))
        required.update(str(p) for p in (target/stage).rglob('*') if p.is_file())
    required.update(str(p) for p in exported.rglob('*') if p.is_file())
    registry=evidence/'projects'
    if registry.is_symlink() or not registry.is_dir(): raise ValueError('project registry missing')
    required.update(str(p) for p in registry.iterdir())
    if not required<=audited: raise ValueError('authority inventory incomplete')
    if not isinstance(manifest,dict) or set(manifest)!={'artifacts'}: raise ValueError('raw manifest malformed')
    seen=inventory_entries(manifest['artifacts']); logs=target/'logs'; actual=set()
    if logs.is_symlink() or not logs.is_dir(): raise ValueError('raw target logs missing')
    for p in logs.rglob('*'):
        if p.is_symlink(): raise ValueError('unsafe target log')
        if p.is_file(): actual.add(str(p))
    if seen!=actual or not actual: raise ValueError('target raw logs incomplete')
    return {'trial_name':name,'trial':trial}


def live_check(evidence,trial_name,trial,*,run=subprocess.run):
    """Repeat current owned-resource checks; caller holds the admission lock."""
    import retained_b_census
    evidence=Path(evidence); eid=evidence.name; reviews=launch.PROJECT/'reviews'
    if not re.fullmatch(r'planar-t044-[0-9a-f]{32}',eid) or not re.fullmatch(r'[A-Za-z0-9_-]+',trial_name): raise ValueError('invalid target identity')
    names={(eid+'-'+trial_name).lower()+'__env',(eid+'-'+trial_name).lower()+'__verifier__trial'}
    # Query both possible labels even if a registration is absent or malformed.
    failures=[]
    for name in sorted(names):
        for kind in (['container','ls','-aq'],['network','ls','-q'],['volume','ls','-q']):
            try:
                result=run(['docker',*kind,'--filter','label=com.docker.compose.project='+name],capture_output=True,text=True,timeout=15)
                if result.returncode or result.stdout.strip(): failures.append(name)
            except (OSError,subprocess.SubprocessError): failures.append(name)
    if failures: raise ValueError('owned resource absence unproven')
    def obj(name): return json.loads(read_safe(evidence/name,reviews.resolve(strict=True)),object_pairs_hook=unique)
    if (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink(): raise ValueError('recovery pending')
    for name in ('launcher-terminal.json','watchdog-terminal.json'):
        value=obj(name)
        if value.get('execution_id')!=eid or value.get('status')!='completed' or type(value.get('return_code')) is not int or value['return_code']!=0: raise ValueError('supervisor termination unproven')
    process=obj('harbor-process.json'); pid=process.get('pid')
    if process.get('execution_id')!=eid or type(pid) is not int or pid<=1: raise ValueError('native process unavailable')
    try: os.killpg(pid,0)
    except ProcessLookupError: pass
    except PermissionError: raise ValueError('native group unknown') from None
    else: raise ValueError('native group active')
    terminal=obj('planar-terminal.json')
    if terminal!={'execution_id':eid,'status':'shell_completed','return_code':0} or type(terminal.get('return_code')) is not int or obj('planar-cleanup.json')!={'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'}:
        raise ValueError('guest terminal or cleanup mismatch')
    if obj('verifier-compose-up.json')!={'execution_id':eid,'command':['up','--detach','--wait','--no-build','--pull','never']}:
        raise ValueError('verifier startup mismatch')
    network=obj('verifier-network.json')
    gate=json.loads(read_safe(Path(obj('retained-admission.json')['gate_path']),reviews.resolve(strict=True)),object_pairs_hook=unique)
    if (set(network)!={'execution_id','container_id','network_mode','image_id'} or network['execution_id']!=eid or network['network_mode']!='none' or network['image_id']!=gate.get('verifier_image_id') or type(network['container_id']) is not str or not re.fullmatch(r'[0-9a-f]{12,64}',network['container_id'])):
        raise ValueError('verifier network or image mismatch')
    registry=evidence/'projects'
    if registry.is_symlink() or not registry.is_dir(): raise ValueError('project registry unavailable')
    found=set()
    for p in registry.iterdir():
        if p.is_symlink() or not p.is_file() or p.stat().st_nlink!=1: raise ValueError('unsafe project record')
        value=json.loads(read_safe(p,reviews.resolve(strict=True)),object_pairs_hook=unique); name=value.get('compose_project')
        if set(value)!={'execution_id','compose_project'} or value['execution_id']!=eid or name not in names or name in found or p.name!=hashlib.sha256(name.encode()).hexdigest()+'.json': raise ValueError('project registry mismatch')
        found.add(name)
    if found!=names: raise ValueError('project registry incomplete')
    census=retained_b_census.check(launch.ROOT,eid,evidence)
    if type(census.get('matching_commands')) is not int or census['matching_commands']!=0 or type(census.get('unresolved')) is not int or census['unresolved']!=0: raise ValueError('command census unresolved')
    from datetime import timezone
    return {**census,'execution_id':eid,'observed_at':datetime.now(timezone.utc).isoformat(),'native_group':'absent','guest_cleanup':'recorded and resources absent','registered_projects':sorted(found),'queried_projects':sorted(names)}


def resolver_identity():
    def git(*args): return subprocess.run(['git',*args],cwd=launch.ROOT,capture_output=True,text=True,check=True,timeout=15).stdout.strip()
    commit=git('rev-parse','HEAD')
    if len(commit)!=40 or git('status','--porcelain','--untracked-files=no') or git('ls-files','--others','--exclude-standard','--','evals/planar/adapters','evals/planar/cases/clean-retained-a-continuation','evals/planar/experiments/codex_run.py'):
        raise ValueError('resolver checkout not clean')
    git('ls-files','--error-unmatch','evals/planar/adapters/clean_retained_release.py')
    return {'commit':commit,'implementation_sha256':launch.source_digest(launch.CASES['clean-retained-a-continuation'])}


def validate_published(start,marker_path,marker):
    try:
        import codex_run
        start,marker_path=Path(start),Path(marker_path); records=start.parent
        if codex_run._ACTIVE_ADMISSION.get()!=records.resolve(): return False
        fields={'schema','type','execution_id','review_path','review_sha256','census_path','census_sha256','review_binding'}
        if not isinstance(marker,dict) or set(marker)!=fields or marker['schema']!='planar-retained-clean-account-marker-v1' or marker['type']!='retained-clean-account-v1' or marker_path!=records/(marker['execution_id']+'.resolved.json'): return False
        reviews=launch.reviews_root().resolve(strict=True)
        if read_safe(marker_path,reviews)!=canonical(marker)+b'\n': return False
        review=safe_path(marker['review_path'],reviews); raw=read_safe(review,reviews); doc=parse(raw)
        if digest(raw)!=marker['review_sha256'] or doc['execution_id']!=marker['execution_id'] or canonical(doc)!=canonical(marker['review_binding']): return False
        census=Path(marker['census_path'])
        if census.parent!=records.resolve() or census.name!=marker['execution_id']+'.retained-clean-census-'+marker['census_sha256']+'.json': return False
        census_raw=read_safe(census,reviews); value=json.loads(census_raw,object_pairs_hook=unique)
        if census_raw!=canonical(value)+b'\n' or digest(canonical(value))!=marker['census_sha256'] or value.get('execution_id')!=marker['execution_id'] or value.get('native_group')!='absent' or type(value.get('matching_commands')) is not int or value['matching_commands']!=0 or type(value.get('unresolved')) is not int or value['unresolved']!=0: return False
        if resolver_identity()!={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}: return False
        evidence=launch.jobs_root()/doc['execution_id']; verified=verify(doc,evidence,start)
        live_check(evidence,verified['trial_name'],verified['trial'])
        return True
    except (OSError,ValueError,TypeError,KeyError,AttributeError,subprocess.SubprocessError): return False


def publish(evidence,review):
    """Publish once only after independent review and fresh locked validation."""
    import sys
    sys.path.insert(0,str(launch.EVALS/'planar/experiments'))
    try:
        import codex_run
        reviews=launch.reviews_root().resolve(strict=True); evidence=Path(evidence); review=safe_path(review,reviews)
        if review.is_relative_to(launch.jobs_root().resolve(strict=True)): raise ValueError('review inside target evidence')
        raw=read_safe(review,reviews); doc=parse(raw)
        if evidence.name!=doc['execution_id']: raise ValueError('execution mismatch')
        with codex_run.admission_scope() as records:
            eid=doc['execution_id']; start=records/(eid+'.start.json'); resolved=records/(eid+'.resolved.json'); closed=records/(eid+'.closed.json')
            if closed.exists() or closed.is_symlink(): raise ValueError('conflicting closed marker')
            verified=verify(doc,evidence,start)
            if resolver_identity()!={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}: raise ValueError('resolver identity mismatch')
            census=live_check(evidence,verified['trial_name'],verified['trial'])
            if read_safe(review,reviews)!=raw: raise ValueError('review bytes changed')
            data=canonical(census)+b'\n'; checksum=digest(canonical(census))
            census_path=records/(eid+'.retained-clean-census-'+checksum+'.json')
            marker={'schema':'planar-retained-clean-account-marker-v1','type':'retained-clean-account-v1','execution_id':eid,'review_path':str(review.resolve(strict=True)),'review_sha256':digest(raw),'census_path':str(census_path),'census_sha256':checksum,'review_binding':doc}
            if resolved.exists() or resolved.is_symlink():
                if resolved.is_symlink() or not resolved.is_file(): raise ValueError('conflicting resolved marker')
                existing=json.loads(read_safe(resolved,reviews),object_pairs_hook=unique)
                if not isinstance(existing,dict) or set(existing)!=set(marker) or any(canonical(existing[k])!=canonical(marker[k]) for k in marker if k not in ('census_path','census_sha256')) or not validate_published(start,resolved,existing): raise ValueError('conflicting resolved marker')
                return {'execution_id':eid,'account':'resolved','outcome':doc['target_status']}
            try: fd=os.open(census_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            except FileExistsError:
                if read_safe(census_path,reviews)!=data: raise ValueError('conflicting census') from None
            else:
                with os.fdopen(fd,'wb') as f: f.write(data); f.flush(); os.fsync(f.fileno())
            fd=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
            try: os.fsync(fd)
            finally: os.close(fd)
            fd,tmp=tempfile.mkstemp(prefix='.retained-clean-',dir=records)
            try:
                with os.fdopen(fd,'wb') as f: f.write(canonical(marker)+b'\n'); f.flush(); os.fsync(f.fileno())
                if read_safe(review,reviews)!=raw: raise ValueError('review bytes changed before publication')
                os.link(tmp,resolved,follow_symlinks=False)
                directory=os.open(records,os.O_RDONLY|os.O_DIRECTORY)
                try: os.fsync(directory)
                finally: os.close(directory)
            finally: os.unlink(tmp)
            return {'execution_id':eid,'account':'resolved','outcome':doc['target_status']}
    finally:
        sys.path.remove(str(launch.EVALS/'planar/experiments'))
