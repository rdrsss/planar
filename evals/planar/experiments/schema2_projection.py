"""Closed, secret-free Harbor 0.26.0 direct-field projection for TS-25.8."""
import importlib.metadata
import json
import math
from enum import Enum
from pathlib import Path
from pydantic import BaseModel
from harbor.models.job.config import JobConfig, RetryConfig
from harbor.models.trial.config import AgentConfig, EnvironmentConfig, VerifierConfig, TaskConfig
from harbor.models.task.config import EnvironmentConfig as TaskEnvironmentConfig

class Invalid(ValueError): pass

# Exact inventories are an explicit compatibility boundary, not runtime-derived policy.
INVENTORY = {
 JobConfig: 'schema_version job_name jobs_dir n_attempts install_only skip_credential_checks timeout_multiplier agent_timeout_multiplier verifier_timeout_multiplier agent_setup_timeout_multiplier environment_build_timeout_multiplier debug n_concurrent_trials quiet retry override_error_policy environment verifier metrics agents user_agent datasets tasks artifacts extra_instruction_paths extra_instructions source_jobs',
 RetryConfig: 'max_retries include_exceptions exclude_exceptions wait_multiplier min_wait_sec max_wait_sec',
 AgentConfig: 'name import_path model_name base_url model_api_key_name model_api_format skip_model_parsing n_concurrent concurrency_group skills override_timeout_sec override_setup_timeout_sec max_timeout_sec resume_trajectory load_trajectory extra_allowed_hosts include_logs exclude_logs kwargs env mcp_servers',
 EnvironmentConfig: 'type import_path force_build delete stream cpu_enforcement_policy memory_enforcement_policy override_cpus override_memory_mb override_storage_mb override_gpus override_tpu suppress_override_warnings mounts extra_docker_compose env kwargs extra_allowed_hosts',
 VerifierConfig: 'override_timeout_sec max_timeout_sec include_logs exclude_logs env import_path kwargs disable extra_allowed_hosts',
 TaskConfig: 'path git_url git_commit_id name ref overwrite download_dir source',
 TaskEnvironmentConfig: 'network_mode allowed_hosts build_timeout_sec docker_image os cpus memory_mb storage_mb gpus gpu_types tpu mcp_servers env skills_dir healthcheck workdir allow_internet',
}
PUBLIC_CONFIG = {'plugins':{'_default':{'enabled':False}},'skills':{'bundled':{'enabled':False}},'features':{'apps':False,'remote_plugin':False,'multi_agent':True}}
SENTINELS = ('<reserved-job-name>','<reserved-job-directory>','<variant-case-instance>','<variant-driver-effort>')

def versions():
    if importlib.metadata.version('harbor') != '0.26.0' or importlib.metadata.version('pydantic') != '2.14.3': raise Invalid('unsupported dependency versions')
    for cls, fields in INVENTORY.items():
        if tuple(cls.model_fields) != tuple(fields.split()): raise Invalid('unsupported model field inventory')

def canonical(value):
    try: return (json.dumps(value,ensure_ascii=True,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n').encode()
    except (TypeError,ValueError): raise Invalid('unsupported canonical value') from None

def project(value, path=''):
    if isinstance(value,BaseModel):
        if type(value) not in INVENTORY or getattr(value,'__pydantic_extra__',None) or getattr(value,'__pydantic_private__',None) or set(value.__dict__) != set(type(value).model_fields): raise Invalid('unknown model, private state or extra')
        return {key:project(getattr(value,key),path+'/'+key) for key in type(value).model_fields}
    if isinstance(value,Enum): return project(value.value,path)
    if isinstance(value,Path): return str(value)
    if value is None or type(value) in (bool,str,int): return value
    if type(value) is float:
        if not math.isfinite(value): raise Invalid('nonfinite number')
        return value
    if type(value) in (list,tuple): return [project(x,path+'/'+str(i)) for i,x in enumerate(value)]
    if type(value) is dict:
        if any(type(k) is not str for k in value): raise Invalid('non-string key')
        return {k:project(v,path+'/'+k) for k,v in value.items()}
    if type(value) in (set,frozenset) and path in ('/retry/include_exceptions','/retry/exclude_exceptions') and all(type(x) is str for x in value): return sorted(value)
    raise Invalid('unsupported value')

def exact(left, right):
    if type(left) is not type(right): return False
    if type(left) is dict: return left.keys() == right.keys() and all(exact(left[k],right[k]) for k in left)
    if type(left) is list: return len(left)==len(right) and all(exact(a,b) for a,b in zip(left,right))
    return left == right

def check_allowlist(job, effort):
    """Check raw values before any projection, persistence, display or hashing."""
    if type(job) is not JobConfig or len(job.agents)!=1 or len(job.tasks)!=1: raise Invalid('unsupported job shape')
    a=job.agents[0]; env=job.environment; verifier=job.verifier
    if (a.import_path != 'smoke_codex:SmokeCodex' or a.model_name != 'gpt-6.1-terra' or a.base_url is not None or a.model_api_key_name is not None or a.model_api_format is not None or a.env or a.mcp_servers or a.skills or a.extra_allowed_hosts or a.load_trajectory is not None or a.resume_trajectory or job.user_agent is not None or job.datasets or job.source_jobs or job.extra_instruction_paths or job.extra_instructions): raise Invalid('unsupported agent channel')
    if effort not in ('medium','high') or not exact(a.kwargs, {'version':'0.169.0','config':PUBLIC_CONFIG,'reasoning_effort':effort}): raise Invalid('unsupported public agent config')
    if env.import_path != 'guarded_docker:GuardedDocker' or not exact(env.kwargs, {'auto_mounting':False}) or env.env or env.mounts or env.extra_docker_compose or env.extra_allowed_hosts or verifier.env or verifier.kwargs or verifier.import_path or verifier.extra_allowed_hosts or verifier.disable: raise Invalid('unsupported environment channel')
    task=job.tasks[0]
    if task.path is None or task.git_url is not None or task.git_commit_id is not None or task.name is not None or task.ref is not None or task.source is not None: raise Invalid('unsupported task source')
    if job.n_attempts != 1 or job.n_concurrent_trials != 1 or job.retry.max_retries != 0: raise Invalid('unsupported execution concurrency')

def normalize(job, *, execution_id, evidence_raw, case_raw, effort):
    versions(); check_allowlist(job,effort)
    if not isinstance(execution_id,str) or len(execution_id)!=32 or any(c not in '0123456789abcdef' for c in execution_id): raise Invalid('invalid execution id')
    if job.job_name != 'planar-t044-'+execution_id or str(job.jobs_dir) != evidence_raw or str(job.tasks[0].path) != case_raw or job.agents[0].kwargs['reasoning_effort'] != effort: raise Invalid('concrete job pointer mismatch')
    full=project(job)
    normalized=project(job)
    normalized['job_name'],normalized['jobs_dir'],normalized['tasks'][0]['path'],normalized['agents'][0]['kwargs']['reasoning_effort'] = SENTINELS
    return full,normalized

def parse_closed(raw, effort, *, trusted):
    """Only accept already complete direct-field JSON; never serialize rejected input."""
    def pairs(items):
        result={}
        for k,v in items:
            if k in result: raise Invalid('duplicate input key')
            result[k]=v
        return result
    try:
        data=json.loads(raw,object_pairs_hook=pairs,parse_constant=lambda _: (_ for _ in ()).throw(Invalid('nonfinite input')))
        # Check the untrusted env/kwargs structure before model construction.
        if not isinstance(data,dict) or len(data.get('agents',[]))!=1: raise Invalid('unsupported input')
        a=data['agents'][0]; e=data['environment']; v=data['verifier']
        if a.get('env') or e.get('env') or v.get('env') or not exact(a.get('kwargs'), {'version':'0.169.0','config':PUBLIC_CONFIG,'reasoning_effort':effort}) or not exact(e.get('kwargs'), {'auto_mounting':False}) or v.get('kwargs'): raise Invalid('unsupported input channel')
        # Compare strict shape, scalar types and all concrete values before
        # Pydantic can coerce or discard any input; trusted is generated locally
        # from the reviewed policy and the reserved execution bindings.
        versions(); check_allowlist(trusted,effort)
        if not exact(data, project(trusted)): raise Invalid('input differs from trusted complete configuration')
        raw_bytes=raw.encode('utf-8') if isinstance(raw,str) else raw
        if raw_bytes != canonical(data): raise Invalid('noncanonical input bytes')
        job=JobConfig.model_validate(data)
        check_allowlist(job,effort)
        if canonical(data) != canonical(project(job)): raise Invalid('noncanonical, coerced, omitted or unknown field')
        return job
    except (KeyError,TypeError,ValueError) as exc:
        if isinstance(exc,Invalid): raise
        raise Invalid('invalid job input') from None

def effective_verifier_environment(original, image_id):
    """Return a copy with the only approved override; does not start Docker."""
    versions()
    if type(original) is not TaskEnvironmentConfig or original.env or original.docker_image is not None:
        raise Invalid('unsupported original verifier environment')
    if getattr(original.network_mode,'value',original.network_mode) != 'no-network':
        raise Invalid('verifier must request no network')
    if not isinstance(image_id,str) or len(image_id)!=71 or not image_id.startswith('sha256:') or any(c not in '0123456789abcdef' for c in image_id[7:]):
        raise Invalid('invalid prepared verifier image')
    effective=original.model_copy(deep=True,update={'docker_image':image_id})
    before,after=project(original),project(effective)
    expected=dict(before); expected['docker_image']=image_id
    if canonical(after)!=canonical(expected): raise Invalid('unexpected verifier override')
    return effective
