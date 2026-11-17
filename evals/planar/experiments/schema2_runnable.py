"""Review-only validation primitives for TS-25.8 runnable-v1 records.

This module neither creates runnable records nor grants admission. Actual image
inspection, shared locking, gate consumption and native dispatch remain required.
"""
import hashlib
from pathlib import Path
import per_variant as pv
import schema2
import schema2_projection as projection
import schema2_reporting

CONTRACT = 'planar-schema2-runnable-v1'
MANIFEST_FIELDS = frozenset(('schema_version','case','contract_version','configuration','configuration_sha256','implementation_sha256','deadline_at','variants','trials','identity','runnable_contract'))
RESERVATION_FIELDS = schema2_reporting.RESERVATION_FIELDS | {'runnable_contract','concrete_binding'}
GATE_FIELDS = schema2.GATE_FIELDS | {'runnable_contract','concrete_job_config_sha256'}
BINDING_FIELDS = frozenset(('binding_version','projection_version','job_config','effective_verifier_environment','path_bindings','caps','verifier_preparation','concrete_job_config_sha256'))
PREPARATION_FIELDS = frozenset(('case_instance_sha256','trusted_inputs_sha256','verifier_code_sha256','build_context_sha256','base_image_id','verifier_image_id','agent_image_id','preparation_evidence_sha256'))
EVIDENCE_FIELDS = (PREPARATION_FIELDS - {'preparation_evidence_sha256'}) | {'schema_version','build_method','build_log_sha256'}
POLICY_FIELDS = frozenset(('policy_version','harbor_version','projection_version','normalized_job_config','roots'))
ROOTS = frozenset(('experiment','reviews','jobs','consumed'))
PATHS = ROOTS | {'candidate','case_instance','evidence'}
VARIANT_FIELDS = frozenset(('variant_id','source','resolved_source','effort','image_id','verifier_image_id','host_version',*schema2.IDENTITIES,'configuration','configuration_sha256'))
CONFIG_FIELDS = frozenset(('config','raw_output_root','canonical_output_root','account_scope_id'))
CONFIG_PAYLOAD_FIELDS = frozenset(('model','variant_efforts','caps','native_config'))
CAP_FIELDS = frozenset(('session_seconds','verifier_seconds','trial_seconds','job_seconds','trials','max_executions'))


def closed(value, fields):
    if not isinstance(value,dict) or set(value)!=fields: raise schema2.Invalid('invalid closed runnable record')

def path_pair(value):
    closed(value,{'raw','resolved'})
    raw,resolved=value['raw'],value['resolved']
    if not isinstance(raw,str) or not isinstance(resolved,str) or not Path(raw).is_absolute() or not Path(resolved).is_absolute() or str(Path(raw).resolve()) != resolved:
        raise schema2.Invalid('runnable path identity mismatch')

def digest(value): return hashlib.sha256(projection.canonical(value)).hexdigest()

def check(root, identity, reservation_id, gate, *, job, original_verifier_environment, preparation_evidence, allow_activated=False):
    """Check review inputs only; return no approval or authorization."""
    root=Path(root)
    m=schema2.validate(root,identity)
    closed(m,MANIFEST_FIELDS)
    if m['runnable_contract'] != CONTRACT or type(m['schema_version']) is not int or m['schema_version'] != 2 or type(m['contract_version']) is not int or m['contract_version'] != 2 or m['case'] != 'planar/native-lineage-portable': raise schema2.Invalid('offline-only or invalid manifest')
    closed(m['configuration'],CONFIG_FIELDS)
    closed(m['configuration']['config'],CONFIG_PAYLOAD_FIELDS)
    closed(m['configuration']['config']['caps'],CAP_FIELDS)
    if any(type(value) is not int or not 0 < value <= 86400 for value in m['configuration']['config']['caps'].values()): raise schema2.Invalid('invalid caps')
    if not isinstance(m['variants'],list) or not m['variants'] or not isinstance(m['trials'],list) or not m['trials']: raise schema2.Invalid('invalid matrix')
    for variant in m['variants']: closed(variant,VARIANT_FIELDS)
    for trial in m['trials']: closed(trial,{'logical_id','variant_id','repetition'})
    ids=[variant['variant_id'] for variant in m['variants']]
    config=m['configuration']['config']
    if len(set(ids)) != len(ids) or config['model'] != 'gpt-6.1-terra' or config['variant_efforts'] != {v['variant_id']:v['effort'] for v in m['variants']} or any(v['effort'] not in ('medium','high') for v in m['variants']): raise schema2.Invalid('invalid variant configuration')
    logical=set()
    for trial in m['trials']:
        if type(trial['repetition']) is not int or trial['repetition'] < 1 or trial['variant_id'] not in ids or trial['logical_id'] != f"{trial['variant_id']}:{trial['repetition']}" or trial['logical_id'] in logical: raise schema2.Invalid('invalid logical trial')
        logical.add(trial['logical_id'])
    path=root/'reservations'/f'{pv.safe_id(reservation_id)}.json'
    r=schema2.read_record(path)
    closed(r,RESERVATION_FIELDS); closed(gate,GATE_FIELDS)
    if r['runnable_contract'] != CONTRACT or gate['runnable_contract'] != CONTRACT: raise schema2.Invalid('offline-only reservation or gate')
    # Delegate unchanged legacy gate checks for actual reservation, roots, expiry,
    # scope, images and frozen input selection; it does not consume or authorize.
    legacy={k:gate[k] for k in schema2.GATE_FIELDS}
    selected=schema2.validate_gate(root,identity,reservation_id,legacy,allow_activated=allow_activated)
    v=selected['variant']; cfg=m['configuration']['config']; policy=cfg['native_config']
    closed(policy,POLICY_FIELDS)
    if type(policy['policy_version']) is not int or policy['policy_version']!=1 or type(policy['projection_version']) is not int or policy['projection_version']!=1 or policy['harbor_version']!='0.26.0': raise schema2.Invalid('unsupported runnable policy')
    closed(policy['roots'],ROOTS)
    for pair in policy['roots'].values(): path_pair(pair)
    binding=r['concrete_binding']; closed(binding,BINDING_FIELDS)
    if type(binding['binding_version']) is not int or binding['binding_version']!=1 or type(binding['projection_version']) is not int or binding['projection_version']!=1: raise schema2.Invalid('unsupported binding')
    closed(binding['path_bindings'],PATHS)
    for pair in binding['path_bindings'].values(): path_pair(pair)
    paths=binding['path_bindings']
    for name in ROOTS:
        if not projection.exact(paths[name],policy['roots'][name]): raise schema2.Invalid('binding root differs from policy')
    if paths['experiment']['raw']!=str(root.absolute()) or paths['candidate']['raw']!=selected['candidate'] or paths['case_instance']['raw']!=selected['instance']:
        raise schema2.Invalid('selected frozen path differs')
    if paths['evidence']['raw'] != str(Path(paths['jobs']['raw'])/('planar-t044-'+reservation_id)):
        raise schema2.Invalid('reserved evidence path differs')
    if paths['consumed']['raw'] != str(Path(paths['reviews']['raw'])/'harbor-t044-consumed'):
        raise schema2.Invalid('consumed root differs')
    if not projection.exact(binding['caps'],cfg['caps']): raise schema2.Invalid('caps differ')
    full,normalized=projection.normalize(job,execution_id=reservation_id,evidence_raw=paths['evidence']['raw'],case_raw=paths['case_instance']['raw'],effort=v['effort'])
    if not projection.exact(full,binding['job_config']) or not projection.exact(normalized,policy['normalized_job_config']): raise schema2.Invalid('job config mismatch')
    effective=projection.effective_verifier_environment(original_verifier_environment,v['verifier_image_id'])
    if not projection.exact(projection.project(effective),binding['effective_verifier_environment']): raise schema2.Invalid('verifier environment mismatch')
    prep=binding['verifier_preparation']; closed(prep,PREPARATION_FIELDS)
    expected={'case_instance_sha256':v['case_instance_sha256'], 'trusted_inputs_sha256':v['trusted_inputs_sha256'], 'verifier_code_sha256':v['verifier_code_sha256'],
        'build_context_sha256':pv.tree_hash(Path(selected['instance'])/'tests'), 'agent_image_id':v['image_id'], 'verifier_image_id':v['verifier_image_id']}
    if any(prep[k]!=value for k,value in expected.items()): raise schema2.Invalid('verifier preparation mismatch')
    schema2.image(prep['base_image_id'])
    evidence=Path(preparation_evidence)
    if evidence.is_symlink() or not evidence.is_file() or evidence.stat().st_size>1024*1024:
        raise schema2.Invalid('missing or unsafe preparation evidence')
    evidence_bytes=evidence.read_bytes()
    try: evidence_record=pv.parse(evidence_bytes,1024*1024)
    except pv.Invalid: raise schema2.Invalid('invalid preparation record') from None
    # This bounded identity-only record is no proof that an image was actually
    # built. Closed non-secret fields are checked before hashing its bytes.
    closed(evidence_record,EVIDENCE_FIELDS)
    if type(evidence_record['schema_version']) is not int or evidence_record['schema_version'] != 1 or evidence_record['build_method'] != 'docker-build-no-pull-network-none-v1' or any(evidence_record[k] != prep[k] for k in PREPARATION_FIELDS - {'preparation_evidence_sha256'}):
        raise schema2.Invalid('preparation record mismatch')
    log=evidence.with_name(evidence.name+'.build.log')
    if log.is_symlink() or not log.is_file() or log.stat().st_size>1024*1024 or not isinstance(evidence_record['build_log_sha256'],str) or not pv.HEX.fullmatch(evidence_record['build_log_sha256']) or schema2.h(log.read_bytes())!=evidence_record['build_log_sha256']:
        raise schema2.Invalid('missing or altered preparation build log')
    if schema2.h(evidence_bytes)!=prep['preparation_evidence_sha256']: raise schema2.Invalid('altered preparation evidence')
    computed=digest({k:value for k,value in binding.items() if k!='concrete_job_config_sha256'})
    if binding['concrete_job_config_sha256'] != computed or gate['concrete_job_config_sha256'] != computed: raise schema2.Invalid('concrete configuration mismatch')
    return {'status':'review-only','authorization':'not granted','gate_consumed':False,'execution_id':reservation_id,'concrete_job_config_sha256':computed}


def derive_policy(job, *, execution_id, evidence_raw, case_raw, effort, roots):
    """Derive a closed policy from a trusted public JobConfig, without writing."""
    closed(roots,ROOTS)
    for pair in roots.values(): path_pair(pair)
    if evidence_raw != str(Path(roots['jobs']['raw'])/('planar-t044-'+execution_id)):
        raise schema2.Invalid('evidence outside reserved job root')
    if roots['consumed']['raw'] != str(Path(roots['reviews']['raw'])/'harbor-t044-consumed'):
        raise schema2.Invalid('consumed root differs')
    _, normalized=projection.normalize(job,execution_id=execution_id,evidence_raw=evidence_raw,case_raw=case_raw,effort=effort)
    return {'policy_version':1,'harbor_version':'0.26.0','projection_version':1,'normalized_job_config':normalized,'roots':roots}


def derive_binding(job, *, execution_id, effort, paths, caps, original_verifier_environment, verifier_preparation):
    """Derive a non-authorizing concrete binding after no-auth preparation."""
    closed(paths,PATHS); closed(caps,CAP_FIELDS); closed(verifier_preparation,PREPARATION_FIELDS)
    for pair in paths.values(): path_pair(pair)
    if any(type(value) is not int or not 0 < value <= 86400 for value in caps.values()): raise schema2.Invalid('invalid finite caps')
    if paths['evidence']['raw'] != str(Path(paths['jobs']['raw'])/('planar-t044-'+execution_id)):
        raise schema2.Invalid('evidence outside reserved job root')
    full,_=projection.normalize(job,execution_id=execution_id,evidence_raw=paths['evidence']['raw'],case_raw=paths['case_instance']['raw'],effort=effort)
    effective=projection.effective_verifier_environment(original_verifier_environment,verifier_preparation['verifier_image_id'])
    # Strict identity formats before hashing; actual image and evidence inspection
    # is a separate required admission boundary.
    for name in ('agent_image_id','verifier_image_id','base_image_id'): schema2.image(verifier_preparation[name])
    for name in PREPARATION_FIELDS-{'agent_image_id','verifier_image_id','base_image_id'}:
        value=verifier_preparation[name]
        if not isinstance(value,str) or not pv.HEX.fullmatch(value): raise schema2.Invalid('invalid preparation hash')
    binding={'binding_version':1,'projection_version':1,'job_config':full,
        'effective_verifier_environment':projection.project(effective),'path_bindings':paths,
        'caps':caps,'verifier_preparation':verifier_preparation}
    binding['concrete_job_config_sha256']=digest(binding)
    return binding


def reserve(root, identity, logical_id, *, expires_at, preparation_evidence,
            predecessor_execution_id=None, retry_reason=None):
    """Create one exact runnable reservation; no gate, auth or job is created."""
    import fcntl, os, stat, subprocess, shutil, sys
    from uuid import uuid4
    from datetime import datetime, timezone
    from harbor.models.job.config import JobConfig
    from harbor.models.task.task import Task
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root)
    if root.is_symlink() or not root.is_dir(): raise schema2.Invalid('unsafe experiment root')
    try: fd=os.open(root/'reservation.lock',os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
    except OSError as exc: raise schema2.Invalid('unsafe reservation lock') from exc
    with os.fdopen(fd,'r+') as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode): raise schema2.Invalid('unsafe reservation lock')
        fcntl.flock(lock,fcntl.LOCK_EX)
        selected=schema2.preflight(root,identity,logical_id); m,v,t=selected['manifest'],selected['variant'],selected['trial']
        closed(m,MANIFEST_FIELDS)
        if m['runnable_contract'] != CONTRACT: raise schema2.Invalid('offline-only manifest')
        policy=m['configuration']['config']['native_config']; closed(policy,POLICY_FIELDS); closed(policy['roots'],ROOTS)
        if policy['policy_version']!=1 or policy['projection_version']!=1 or policy['harbor_version']!='0.26.0': raise schema2.Invalid('unsupported policy')
        expected={'experiment':root.absolute(),'reviews':launch.reviews_root().absolute(),'jobs':launch.jobs_root().absolute(),'consumed':(launch.reviews_root()/'harbor-t044-consumed').absolute()}
        for name,path in expected.items():
            if policy['roots'][name] != {'raw':str(path),'resolved':str(path.resolve())}: raise schema2.Invalid('runnable scope drift')
        if m['configuration']['account_scope_id'] != str((launch.reviews_root()/'planar-account-admission-v1').resolve()): raise schema2.Invalid('account scope drift')
        expiry=schema2.utc(expires_at)
        if expiry <= datetime.now(timezone.utc) or expiry > schema2.utc(m['deadline_at']): raise schema2.Invalid('reservation expiry')
        existing=[schema2.read_record(p) for p in (root/'reservations').glob('*.json')]
        attempts=[r for r in existing if r['logical_id']==logical_id]
        if attempts:
            leaves=[r for r in attempts if r['execution_id'] not in {x['predecessor_execution_id'] for x in attempts}]
            if len(leaves)!=1 or predecessor_execution_id!=leaves[0]['execution_id'] or not isinstance(retry_reason,str) or not retry_reason.strip(): raise schema2.Invalid('explicit latest retry required')
            if schema2.read_record(root/'executions'/predecessor_execution_id/'terminal.json').get('status') not in ('incomplete','error'): raise schema2.Invalid('predecessor not retryable')
        elif predecessor_execution_id is not None or retry_reason is not None: raise schema2.Invalid('unexpected retry fields')
        limit=m['configuration']['config']['caps']['max_executions']
        if type(limit) is not int or not 0<limit<=1000 or len(existing)>=limit: raise schema2.Invalid('execution budget exhausted')
        evidence=Path(preparation_evidence)
        if evidence.is_symlink() or not evidence.is_file() or evidence.stat().st_size>1024*1024 or not evidence.resolve().is_relative_to(launch.reviews_root().resolve()): raise schema2.Invalid('unsafe preparation evidence')
        data=evidence.read_bytes()
        try: evidence_record=pv.parse(data,1024*1024)
        except pv.Invalid: raise schema2.Invalid('invalid preparation evidence') from None
        closed(evidence_record,EVIDENCE_FIELDS)
        if type(evidence_record['schema_version']) is not int or evidence_record['schema_version']!=1 or evidence_record['build_method']!='docker-build-no-pull-network-none-v1': raise schema2.Invalid('preparation schema')
        log=evidence.with_name(evidence.name+'.build.log')
        if log.is_symlink() or not log.is_file() or log.stat().st_size>1024*1024 or not isinstance(evidence_record['build_log_sha256'],str) or not pv.HEX.fullmatch(evidence_record['build_log_sha256']) or schema2.h(log.read_bytes())!=evidence_record['build_log_sha256']: raise schema2.Invalid('preparation build log mismatch')
        prep={k:evidence_record[k] for k in PREPARATION_FIELDS-{'preparation_evidence_sha256'}}
        expected_prep={'case_instance_sha256':v['case_instance_sha256'],'trusted_inputs_sha256':v['trusted_inputs_sha256'],'verifier_code_sha256':v['verifier_code_sha256'],'build_context_sha256':pv.tree_hash(Path(selected['instance'])/'tests'),'agent_image_id':v['image_id'],'verifier_image_id':v['verifier_image_id']}
        if any(prep[k]!=value for k,value in expected_prep.items()): raise schema2.Invalid('preparation identity mismatch')
        for key in ('base_image_id','agent_image_id','verifier_image_id'): schema2.image(prep[key])
        prep['preparation_evidence_sha256']=schema2.h(data)
        docker=shutil.which('docker')
        if not docker: raise schema2.Invalid('docker unavailable')
        for key in ('base_image_id','agent_image_id','verifier_image_id'):
            result=subprocess.run([docker,'image','inspect','--format','{{.Id}}',prep[key]],capture_output=True,text=True,timeout=15)
            if result.returncode or result.stdout.strip()!=prep[key]: raise schema2.Invalid('prepared image unavailable')
        eid=uuid4().hex; evidence_dir=expected['jobs']/('planar-t044-'+eid)
        job=JobConfig.model_validate(launch.config_for('planar-t044-'+eid,evidence_dir,Path(selected['instance']),v['effort']))
        roots=policy['roots']; paths={**roots,'candidate':{'raw':selected['candidate'],'resolved':str(Path(selected['candidate']).resolve())},'case_instance':{'raw':selected['instance'],'resolved':str(Path(selected['instance']).resolve())},'evidence':{'raw':str(evidence_dir),'resolved':str(evidence_dir.resolve())}}
        binding=derive_binding(job,execution_id=eid,effort=v['effort'],paths=paths,caps=m['configuration']['config']['caps'],original_verifier_environment=Task(Path(selected['instance'])).config.verifier.environment,verifier_preparation=prep)
        _,normalized=projection.normalize(job,execution_id=eid,evidence_raw=str(evidence_dir),case_raw=selected['instance'],effort=v['effort'])
        if not projection.exact(normalized,policy['normalized_job_config']): raise schema2.Invalid('runnable JobConfig policy mismatch')
        after=schema2.preflight(root,identity,logical_id)
        if after != selected or evidence.is_symlink() or evidence.read_bytes()!=data or schema2.utc(expires_at)<=datetime.now(timezone.utc): raise schema2.Invalid('preparation drift before reservation')
        r={'schema_version':2,'state':'planned','reservation_id':eid,'execution_id':eid,'experiment_identity':identity,**t,
            'predecessor_execution_id':predecessor_execution_id,'retry_reason_sha256':schema2.h(retry_reason.encode()) if retry_reason else None,
            **{key:v[key] for key in schema2.IDENTITIES},'configuration_sha256':v['configuration_sha256'],'implementation_sha256':m['implementation_sha256'],
            'image_id':v['image_id'],'verifier_image_id':v['verifier_image_id'],'host_version':v['host_version'],'account_scope_id':m['configuration']['account_scope_id'],
            'canonical_output_root':m['configuration']['canonical_output_root'],'expires_at':expires_at,'runnable_contract':CONTRACT,'concrete_binding':binding}
        schema2.write_new(root/'reservations'/f'{eid}.json',r)
        return r


def check_gate(root, identity, reservation_id, gate, *, preparation_evidence, allow_activated=False):
    """Reconstruct concrete inputs and compare a gate, without consuming it."""
    import sys
    from harbor.models.job.config import JobConfig
    from harbor.models.task.task import Task
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root); pv.safe_id(reservation_id)
    r=schema2.read_record(root/'reservations'/f'{reservation_id}.json')
    closed(r,RESERVATION_FIELDS)
    selected=schema2.preflight(root,identity,r['logical_id']); v=selected['variant']
    policy=selected['manifest']['configuration']['config']['native_config']; closed(policy,POLICY_FIELDS)
    evidence=Path(policy['roots']['jobs']['raw'])/('planar-t044-'+reservation_id)
    job=JobConfig.model_validate(launch.config_for('planar-t044-'+reservation_id,evidence,Path(selected['instance']),v['effort']))
    original=Task(Path(selected['instance'])).config.verifier.environment
    return check(root,identity,reservation_id,gate,job=job,original_verifier_environment=original,preparation_evidence=preparation_evidence,allow_activated=allow_activated)


def prepare_image(root, identity, logical_id, *, output):
    """Explicit no-auth offline build and immutable evidence; no gate or trial."""
    import os, shutil, subprocess, sys, tempfile
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
    import launch
    root=Path(root); output=Path(output)
    selected=schema2.preflight(root,identity,logical_id); m,v=selected['manifest'],selected['variant']
    if m.get('runnable_contract') != CONTRACT: raise schema2.Invalid('offline-only manifest')
    reviews=launch.reviews_root()
    if reviews.is_symlink() or output.is_symlink() or output.exists() or not output.parent.is_dir() or not output.parent.resolve().is_relative_to(reviews.resolve()): raise schema2.Invalid('unsafe preparation destination')
    log_path=output.with_name(output.name+'.build.log')
    if log_path.is_symlink() or log_path.exists(): raise schema2.Invalid('preparation log exists')
    context=Path(selected['instance'])/'tests'; context_sha=pv.tree_hash(context)
    dockerfile=(context/'Dockerfile').read_text()
    base_ref='python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c'
    if dockerfile.splitlines()[0] != 'FROM '+base_ref: raise schema2.Invalid('unexpected pinned verifier base')
    docker=shutil.which('docker')
    if not docker: raise schema2.Invalid('docker unavailable')
    # Only Docker context discovery uses the user's normal CLI context. Build
    # runs with an empty temporary Docker config and no auth/model variables.
    host=subprocess.run([docker,'context','inspect','--format','{{.Endpoints.docker.Host}}'],capture_output=True,text=True,timeout=15)
    if host.returncode or not host.stdout.strip(): raise schema2.Invalid('docker context unavailable')
    with tempfile.TemporaryDirectory(prefix='planar-verifier-noauth-') as temp:
        temp=Path(temp); config=temp/'config'; config.mkdir(mode=0o700)
        env={'PATH':os.environ.get('PATH',''),'DOCKER_HOST':host.stdout.strip(),'DOCKER_CONFIG':str(config),'DOCKER_BUILDKIT':'0'}
        def inspect(image):
            result=subprocess.run([docker,'image','inspect','--format','{{.Id}}',image],capture_output=True,text=True,timeout=15,env=env)
            if result.returncode: raise schema2.Invalid('local image inspection failed')
            return schema2.image(result.stdout.strip())
        base_id=inspect(base_ref)
        if inspect(v['image_id'])!=v['image_id']: raise schema2.Invalid('agent image mismatch')
        iid=temp/'image-id'
        result=subprocess.run([docker,'build','--pull=false','--network=none','--iidfile',str(iid),str(context)],capture_output=True,timeout=600,env=env)
        log=result.stdout+b'\n'+result.stderr
        if result.returncode or len(log)>1024*1024 or not iid.is_file(): raise schema2.Invalid('verifier build failed')
        actual=iid.read_text().strip()
        if actual!=v['verifier_image_id'] or inspect(actual)!=actual: raise schema2.Invalid('prepared verifier image differs from plan')
    if schema2.preflight(root,identity,logical_id)!=selected or pv.tree_hash(context)!=context_sha: raise schema2.Invalid('frozen input drift during build')
    record={'schema_version':1,'build_method':'docker-build-no-pull-network-none-v1','build_log_sha256':schema2.h(log),
        'case_instance_sha256':v['case_instance_sha256'],'trusted_inputs_sha256':v['trusted_inputs_sha256'],
        'verifier_code_sha256':v['verifier_code_sha256'],'build_context_sha256':context_sha,
        'base_image_id':base_id,'verifier_image_id':actual,'agent_image_id':v['image_id']}
    fd=os.open(log_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'wb') as stream: stream.write(log); stream.flush(); os.fsync(stream.fileno())
    schema2.write_new(output,record)
    return {'status':'prepared','evidence':str(output),'evidence_sha256':schema2.h(pv.canonical(record)),
            'build_log':str(log_path),'verifier_image_id':actual,'authorization':'not granted'}
