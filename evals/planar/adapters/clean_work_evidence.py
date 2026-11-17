"""Static reviewed authority for a single clean-work native trial."""
import hashlib
import json
import re
import subprocess
from datetime import datetime
from pathlib import Path

from harbor.models.job.config import JobConfig
from harbor.models.job.result import JobResult
from harbor.models.trial.result import TrialResult
from candidate import inventory
import launch
from clean_work_outcomes import observe
from retained_b_early_evidence import read_safe
from terminal_failure import historical_inventory
from terminal_failure_schema import unique


def canonical(obj): return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
def digest(raw): return hashlib.sha256(raw).hexdigest()


def validate_native(job_raw, trial_raw, config_raw, *, job_id, trial_id, trial_name, job_dir, task_name, actual, strategy):
    job=JobResult.model_validate_json(job_raw); trial=TrialResult.model_validate_json(trial_raw); config=JobConfig.model_validate_json(config_raw)
    if (str(job.id)!=job_id or str(trial.id)!=trial_id or str(trial.config.job_id)!=job_id
        or trial.trial_name!=trial_name or trial.config.trial_name!=trial_name
        or trial.task_name!=task_name or str(trial.config.trials_dir)!=str(job_dir)
        or len(config.agents)!=1 or len(config.tasks)!=1
        or trial.config.agent!=config.agents[0] or trial.config.task!=config.tasks[0]
        or getattr(trial.task_id,'path',None)!=config.tasks[0].path
        or trial.config.environment!=config.environment or trial.config.verifier!=config.verifier):
        raise ValueError('native trial configuration mismatch')
    if job.trial_results and (len(job.trial_results)!=1 or job.trial_results[0]!=trial):
        raise ValueError('native job trial results conflict')
    if strategy!='final': raise ValueError('unsupported native reward strategy')
    parsed=json.loads(trial_raw,object_pairs_hook=unique)
    steps=parsed.get('step_results')
    if not isinstance(steps,list) or len(steps)!=2 or [s.get('step_name') for s in steps]!=['worker','review']:
        raise ValueError('native step order mismatch')
    aggregate=parsed.get('verifier_result')
    if 'verifier_result' not in parsed: raise ValueError('native aggregate unavailable')
    expected=steps[-1].get('verifier_result')
    if canonical(aggregate)!=canonical(expected): raise ValueError('native aggregate reward mismatch')
    if actual['review'] is not None:
        if not isinstance(aggregate,dict) or set(aggregate)!={'rewards'} or not isinstance(aggregate['rewards'],dict) or set(aggregate['rewards'])!={'reward'}:
            raise ValueError('native aggregate reward malformed')
        val=aggregate['rewards']['reward']
        if type(val) not in (int,float) or val!=actual['review']['reward']: raise ValueError('native aggregate reward mismatch')
    elif aggregate is not None:
        raise ValueError('native aggregate reward mismatch')
    parsed_job=json.loads(job_raw,object_pairs_hook=unique)
    evals=parsed_job.get('stats',{}).get('evals')
    if not isinstance(evals,dict) or len(evals)!=1: raise ValueError('native job grade inventory unavailable')
    entry=next(iter(evals.values()))
    if not isinstance(entry,dict) or type(entry.get('n_trials')) is not int or entry['n_trials']!=1: raise ValueError('native job grade inventory mismatch')
    rewards=entry.get('reward_stats')
    if not isinstance(rewards,dict): raise ValueError('native job grade inventory unavailable')
    if aggregate is None:
        if rewards: raise ValueError('native job grade mismatch')
    else:
        if set(rewards)!={'reward'} or not isinstance(rewards['reward'],dict) or len(rewards['reward'])!=1:
            raise ValueError('native job grade inventory mismatch')
        key,names=next(iter(rewards['reward'].items()))
        try: score=float(key)
        except (TypeError,ValueError): raise ValueError('native job grade malformed') from None
        if score!=actual['review']['reward'] or names!=[trial_name]: raise ValueError('native job grade mismatch')
    return job,trial


def validate_final_capture(clean, case, raw):
    final=clean/'final'; descriptor=final/'snapshot-manifest.json'; receipt=clean/'final-receipt.json'
    inventory=json.loads(raw(descriptor),object_pairs_hook=unique)
    record=json.loads(raw(receipt),object_pairs_hook=unique)
    container=json.loads(raw(clean/'container.json'),object_pairs_hook=unique)
    active=json.loads(raw(clean/'active.json'),object_pairs_hook=unique)
    if (not isinstance(record,dict) or set(record)!={'stage','wall_ns','monotonic_ns','manifest_sha256'} or record['stage']!='final'
        or type(record['wall_ns']) is not int or record['wall_ns']<=0 or type(record['monotonic_ns']) is not int or record['monotonic_ns']<=0
        or record['manifest_sha256']!=digest(raw(descriptor))): raise ValueError('final capture receipt mismatch')
    if (not isinstance(container,dict) or set(container)!={'container_id','baseline'} or not isinstance(container['container_id'],str)
        or not re.fullmatch(r'[0-9a-f]{64}',container['container_id']) or not isinstance(container['baseline'],list)
        or not container['baseline'] or any(type(x) is not str or not x for x in container['baseline']) or active!={'step':'review'}):
        raise ValueError('final capture boundary unavailable')
    if not isinstance(inventory,dict) or not inventory or len(inventory)>256: raise ValueError('final capture manifest malformed')
    found=set()
    for path in final.rglob('*'):
        if path.is_symlink() or not (path.is_dir() or path.is_file()): raise ValueError('unsafe final capture entry')
        if path.is_file() and path!=descriptor: found.add(path.relative_to(final).as_posix())
    if found!=set(inventory): raise ValueError('final capture inventory mismatch')
    for name,item in inventory.items():
        if not isinstance(name,str) or name.startswith('/') or any(x in ('','.','..') for x in name.split('/')):
            raise ValueError('unsafe final capture path')
        data=raw(final/name)
        if not isinstance(item,dict) or set(item)!={'size','sha256'} or type(item['size']) is not int or item['size']!=len(data) or item['sha256']!=digest(data):
            raise ValueError('final capture content mismatch')
    projection=json.loads((case/'projection.json').read_text())['files']
    required={name for name in projection if name.startswith('home/')} | {'repository/README.md','repository/.git/HEAD','repository/.git/index','host/git-head.txt','host/git-stage.txt','host/git-status.txt','host/task-diff.txt','host/repository-inventory.json'}
    if not required<=found: raise ValueError('final capture missing required files')
    for name,expected in projection.items():
        if name in found and name!='home/projects/revision-fault/tasks.md' and digest(raw(final/name))!=expected:
            raise ValueError('final capture immutable input changed')
    return {str(descriptor),str(receipt),str(clean/'container.json'),str(clean/'active.json')} | {str(final/name) for name in found}


def verify(doc, evidence, start):
    evidence,start=Path(evidence),Path(start); reviews=launch.reviews_root().resolve(strict=True)
    eid=doc['execution_id']
    if evidence.is_symlink() or evidence.resolve(strict=True).parent!=launch.jobs_root().resolve(strict=True) or evidence.name!=eid:
        raise ValueError('execution path mismatch')
    def raw(path): return read_safe(Path(path),reviews)
    def obj(path): return json.loads(raw(path),object_pairs_hook=unique)
    start_raw=raw(start); shared=json.loads(start_raw,object_pairs_hook=unique)
    if digest(start_raw)!=doc['start_sha256'] or start.name!=eid+'.start.json' or shared.get('case')!='clean-work-review' or shared.get('execution_id')!=eid or shared.get('status')!='awaiting-independent-bound-raw-audit':
        raise ValueError('start binding mismatch')
    gate_path=Path(doc['gate_path']); gate_raw=raw(gate_path); gate=json.loads(gate_raw,object_pairs_hook=unique)
    if shared.get('gate_path')!=str(gate_path) or digest(gate_raw)!=doc['gate_sha256'] or shared.get('gate_sha256')!=doc['gate_sha256'] or canonical(shared.get('gate_authority'))!=canonical(gate):
        raise ValueError('gate binding mismatch')
    if gate.get('verdict')!='approve' or gate.get('purpose')!='one-diagnostic-attempt' or type(gate.get('max_attempts')) is not int or gate['max_attempts']!=1:
        raise ValueError('gate authority invalid')
    if gate.get('execution_id')!=eid or gate.get('case')!='clean-work-review' or gate.get('account_scope_id')!=str(start.parent.parent) or gate.get('model_name')!='gpt-6.1-terra' or gate.get('reasoning_effort')!='medium':
        raise ValueError('gate scope mismatch')
    try:
        if datetime.fromisoformat(gate['absolute_deadline']).tzinfo is None: raise ValueError()
    except (TypeError,KeyError,ValueError): raise ValueError('invalid gate deadline') from None
    for key in ('case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256'):
        if type(gate.get(key)) is not str or gate[key]!=doc[key]: raise ValueError('gate source binding mismatch')
    if gate.get('implementation_sha256')!=doc['historical_implementation_sha256']:
        raise ValueError('historical gate mismatch')
    consumed=reviews/'harbor-t044-consumed'/digest(canonical(gate))
    if raw(consumed)!=b'reserved\n': raise ValueError('gate consumption unproven')
    launcher=obj(evidence/'launcher.json'); prereq=launcher['prerequisites']
    for key in ('execution_id','case_sha256','candidate_sha256','configuration_sha256'):
        if launcher.get(key)!=doc[key]: raise ValueError('launcher binding mismatch')
    if launcher.get('case')!='clean-work-review' or launcher.get('implementation_sha256')!=doc['historical_implementation_sha256'] or launcher.get('review_sha256')!=doc['gate_sha256']:
        raise ValueError('launcher authority mismatch')
    budgets=prereq.get('budgets_seconds')
    if not isinstance(budgets,dict) or budgets.get('job')!=1050 or budgets.get('step_agent')!=[400,400] or type(budgets.get('verifier')) not in (int,float) or budgets['verifier']!=60:
        raise ValueError('reviewed time limits differ')
    case=Path(prereq['case_read_root']); candidate=Path(prereq['package_read_root'])
    if launch.case_inventory(case)!=doc['case_sha256'] or inventory(candidate)['sha256']!=doc['candidate_sha256']:
        raise ValueError('frozen inputs changed')
    historical_inventory(doc['historical_commit'],doc['historical_tree'],doc['historical_implementation_sha256'],case='clean-work-review')
    resolver_tree=subprocess.run(['git','rev-parse',doc['resolver_commit']+'^{tree}'],cwd=launch.ROOT,capture_output=True,text=True,check=True,timeout=15).stdout.strip()
    for name in ('clean_work_release.py','clean_work_release_schema.py','clean_work_evidence.py','clean_work_live.py','clean_work_resources.py','clean_work_outcomes.py'):
        subprocess.run(['git','cat-file','-e',doc['resolver_commit']+':evals/planar/adapters/'+name],cwd=launch.ROOT,capture_output=True,check=True,timeout=15)
    historical_inventory(doc['resolver_commit'],resolver_tree,doc['resolver_implementation_sha256'],case='clean-work-review')
    config_raw=raw(evidence/'job-config.json'); config=json.loads(config_raw,object_pairs_hook=unique)
    if digest(canonical(config))!=doc['effective_configuration_sha256']:
        raise ValueError('effective configuration mismatch')
    if prereq.get('effective_configuration_sha256')!=doc['effective_configuration_sha256'] or prereq.get('reserved_execution_id')!=eid or prereq.get('absolute_deadline')!=gate.get('absolute_deadline'):
        raise ValueError('launcher effective binding mismatch')
    agent=config['agents'][0]
    if len(config['agents'])!=1 or agent.get('model_name')!='gpt-6.1-terra' or agent.get('kwargs',{}).get('reasoning_effort')!='medium':
        raise ValueError('selected model mismatch')
    task=launch.Task(case).config
    if (launcher.get('image_id')!=gate.get('image_id') or gate.get('image_id')!=task.environment.docker_image
        or task.verifier.environment is None or gate.get('verifier_image_id')!=task.verifier.environment.docker_image):
        raise ValueError('image binding mismatch')
    native=obj(evidence/'native-trial-start.json'); job_dir=evidence/eid
    if native.get('job_id')!=doc['job_id'] or native.get('trial_id')!=doc['trial_id'] or native.get('job_dir')!=str(job_dir):
        raise ValueError('native start mismatch')
    trial_name=native.get('trial_name')
    if not isinstance(trial_name,str) or not trial_name or '/' in trial_name or trial_name in ('.','..'):
        raise ValueError('native trial name invalid')
    native_config=raw(job_dir/'config.json')
    json.loads(native_config,object_pairs_hook=unique)
    if JobConfig.model_validate_json(config_raw)!=JobConfig.model_validate_json(native_config):
        raise ValueError('submitted and native JobConfigs differ')
    job_raw=raw(job_dir/'result.json'); job=json.loads(job_raw,object_pairs_hook=unique)
    trials=list(job_dir.glob('*/result.json'))
    if len(trials)!=1 or trials[0].parent.name!=trial_name:
        raise ValueError('trial inventory ambiguous')
    trial_raw=raw(trials[0]); trial=json.loads(trial_raw,object_pairs_hook=unique)
    if digest(job_raw)!=doc['job_result_sha256'] or digest(trial_raw)!=doc['trial_result_sha256']:
        raise ValueError('native result bytes changed')
    if job.get('id')!=doc['job_id'] or type(job.get('n_total_trials')) is not int or job['n_total_trials']!=1 or trial.get('id')!=doc['trial_id'] or trial.get('config',{}).get('job_id')!=doc['job_id']:
        raise ValueError('native result identity mismatch')
    stats=job.get('stats')
    if not isinstance(stats,dict) or any(type(stats.get(k)) is not int or stats[k]!=0 for k in ('n_running_trials','n_pending_trials')):
        raise ValueError('native job termination unproven')
    try:
        if datetime.fromisoformat(trial['finished_at'].replace('Z','+00:00')).tzinfo is None: raise ValueError()
    except (AttributeError,TypeError,KeyError,ValueError): raise ValueError('native trial termination unproven') from None
    actual=observe(trials[0].parent,reviews,trial)
    validate_native(job_raw,trial_raw,config_raw,job_id=doc['job_id'],trial_id=doc['trial_id'],trial_name=trial_name,job_dir=job_dir,task_name=launch.Task(case).name,actual=actual,strategy=task.multi_step_reward_strategy.value)
    if canonical(actual)!=canonical(doc['steps']):
        raise ValueError('reviewed verifier outcomes differ')
    for name,value in actual.items():
        if (value is None)!=((name+' verifier output') in doc['missing_artifacts']):
            raise ValueError('verifier output declaration mismatch')
    if 'exception_info' not in trial: raise ValueError('native exception state unavailable')
    exception=trial['exception_info']
    if exception is not None and not isinstance(exception,dict): raise ValueError('native exception malformed')
    serialized=None if exception is None else canonical(exception).decode()
    if serialized!=doc['exception']:
        raise ValueError('native exception differs')
    if doc['terminal_class']=='partial_failure' and exception is None: raise ValueError('partial failure absent')
    if doc['terminal_class']!='partial_failure' and exception is not None: raise ValueError('unreviewed native exception')
    if doc['terminal_class']=='completed' and (None in actual.values() or any(v['status']=='incomplete' for v in actual.values())):
        raise ValueError('completed outcome unsupported')
    if doc['terminal_class']=='incomplete' and not any(v is not None and v['status']=='incomplete' for v in actual.values()):
        raise ValueError('incomplete outcome unsupported')
    authority_raw=raw(doc['authority_path']); manifest_raw=raw(doc['raw_manifest_path'])
    if digest(authority_raw)!=doc['authority_sha256'] or digest(manifest_raw)!=doc['raw_manifest_sha256']:
        raise ValueError('reviewed authority bytes changed')
    authority=json.loads(authority_raw,object_pairs_hook=unique)
    if (authority.get('execution_id')!=eid or authority.get('job_id')!=doc['job_id'] or authority.get('trial_id')!=doc['trial_id']
        or authority.get('full_runtime_authority_revalidated') is not True or authority.get('submitted_actual_jobconfigs_equal') is not True):
        raise ValueError('independent runtime authority mismatch')
    artifacts=authority.get('artifacts'); audited=set()
    if not isinstance(artifacts,list): raise ValueError('authority inventory unavailable')
    for item in artifacts:
        if not isinstance(item,dict) or set(item)!={'path','sha256'} or type(item['path']) is not str or item['path'] in audited:
            raise ValueError('authority inventory ambiguous')
        audited.add(item['path'])
        if digest(raw(item['path']))!=item['sha256']: raise ValueError('authority artifact changed')
    required={str(evidence/x) for x in ('launcher.json','job-config.json','native-trial-start.json','launcher-terminal.json','watchdog-terminal.json','harbor-process.json','planar-cleanup.json')}
    required.update((str(start),str(gate_path),str(job_dir/'config.json'),str(job_dir/'result.json'),str(trials[0])))
    projects=evidence/'projects'
    if projects.is_symlink() or not projects.is_dir(): raise ValueError('project registry unavailable')
    required.update(str(path) for path in projects.iterdir())
    for name in ('worker','review'):
        receipt=evidence/('verifier-compose-up-'+name+'.json')
        if receipt.is_symlink(): raise ValueError('unsafe verifier receipt')
        if receipt.exists(): required.add(str(receipt))
    for name,value in actual.items():
        if value is not None:
            required.update(str(trials[0].parent/'steps'/name/'verifier'/f) for f in ('status.json','reward.txt'))
    final=evidence/'clean-revision'/'final'
    if final.is_symlink() or type(authority.get('final_host_capture_exists')) is not bool or authority['final_host_capture_exists']!=final.is_dir():
        raise ValueError('final host capture mismatch')
    if final.is_dir() == ('final host capture' in doc['missing_artifacts']):
        raise ValueError('final capture declaration mismatch')
    if final.is_dir(): required.update(validate_final_capture(evidence/'clean-revision',case,raw))
    if not required<=audited: raise ValueError('authority inventory incomplete')
    manifest=json.loads(manifest_raw,object_pairs_hook=unique)
    if not isinstance(manifest,dict) or set(manifest)!={'artifacts'} or not isinstance(manifest['artifacts'],list):
        raise ValueError('raw inventory malformed')
    logs=evidence/'clean-revision'/'logs'; seen=set()
    for item in manifest['artifacts']:
        if not isinstance(item,dict) or set(item)!={'path','sha256'} or type(item['path']) is not str or item['path'] in seen:
            raise ValueError('raw inventory ambiguous')
        path=Path(item['path']); seen.add(item['path'])
        if not path.is_relative_to(logs) or digest(raw(path))!=item['sha256']:
            raise ValueError('raw evidence changed')
    found=set()
    if logs.exists():
        if logs.is_symlink() or not logs.is_dir(): raise ValueError('unsafe logs')
        for path in logs.rglob('*'):
            if path.is_symlink(): raise ValueError('unsafe raw log')
            if path.is_file(): found.add(str(path))
    if found!=seen: raise ValueError('raw inventory incomplete')
    for name in ('worker','review'):
        step_logs=logs/name
        if step_logs.is_symlink(): raise ValueError('unsafe step logs')
        label=name+' native logs'
        exists=step_logs.is_dir() and any(path.is_relative_to(step_logs) for path in map(Path,found))
        if exists == (label in doc['missing_artifacts']):
            raise ValueError('step log presence declaration mismatch')
    return {'trial_name':trial_name,'trial':trial}
