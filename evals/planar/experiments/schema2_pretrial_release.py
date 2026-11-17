"""Account-only resolution of one reviewed schema-2 pre-native terminal shape."""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

import controls
import per_variant as pv
import schema2
import schema2_runnable
import schema2_projection
import schema2_execution
import launch
import retained_b_census
from retained_b_early_evidence import read_safe, safe_path
from harbor.models.job.config import JobConfig

FIELDS={'schema','decision','phase','terminal_class','native_job_id','native_trial_id','verifier_result','model_result',
        'experiment_root','experiment_identity','reservation_id','gate_path','gate_sha256','preparation_path',
        'independent_audit_path','independent_audit_sha256','historical_commit','historical_tree','historical_implementation_sha256',
        'resolver_commit','resolver_implementation_sha256','artifacts','reviewer','independence_basis','reviewed_at','independent_review'}
EVIDENCE_FILES={'schema2-context.json','job-config.json','launcher.json','launcher-command.json','harbor-process.json','launcher-terminal.json','watchdog-terminal.json'}
EXEC_FILES={'start.json','dispatch.json','launcher-result.json','terminal.json','account-stop.json'}


def canonical(x): return pv.canonical(x)
def h(data): return hashlib.sha256(data).hexdigest()


def parse(raw):
    doc=pv.parse(raw,4*1024*1024)
    if not isinstance(doc,dict) or set(doc)!=FIELDS: raise ValueError('pretrial review schema mismatch')
    if (doc['schema']!='planar-schema2-pretrial-release-v1' or doc['decision']!='release-account' or doc['phase']!='before-native-trial'
        or doc['terminal_class']!='incomplete' or any(doc[k] is not None for k in ('native_job_id','native_trial_id','verifier_result','model_result'))
        or doc['independent_review'] is not True): raise ValueError('unsupported pretrial review')
    if not re.fullmatch(r'[0-9a-f]{32}',doc['reservation_id']): raise ValueError('invalid reservation')
    for key in ('experiment_identity','gate_sha256','independent_audit_sha256','historical_implementation_sha256','resolver_implementation_sha256'):
        if not isinstance(doc[key],str) or not re.fullmatch(r'[0-9a-f]{64}',doc[key]): raise ValueError('invalid digest')
    for key in ('historical_commit','historical_tree','resolver_commit'):
        if not isinstance(doc[key],str) or not re.fullmatch(r'[0-9a-f]{40}',doc[key]): raise ValueError('invalid commit')
    for key in ('reviewer','independence_basis','reviewed_at'):
        if type(doc[key]) is not str or not doc[key].strip(): raise ValueError('missing review provenance')
    schema2.utc(doc['reviewed_at'])
    for key in ('experiment_root','gate_path','preparation_path','independent_audit_path'):
        p=Path(doc[key]);
        if not p.is_absolute() or '..' in p.parts: raise ValueError('unsafe authority path')
    artifacts=doc['artifacts']
    if not isinstance(artifacts,dict) or not artifacts or any(type(k) is not str or type(v) is not str or not re.fullmatch(r'[0-9a-f]{64}',v) for k,v in artifacts.items()): raise ValueError('invalid authority inventory')
    return doc


def historical_components(commit,tree):
    actual=subprocess.run(['git','rev-parse',commit+'^{tree}'],cwd=launch.ROOT,check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    if actual!=tree: raise ValueError('historical tree mismatch')
    paths=['evals/pyproject.toml','evals/uv.lock','evals/README.md','evals/planar/README.md','evals/planar/adapters','evals/planar/cases/plugin-smoke','evals/planar/experiments']
    archive=subprocess.run(['git','archive','--format=tar',commit,*paths],cwd=launch.ROOT,check=True,capture_output=True,timeout=30).stdout
    files={}; seen=set()
    with tarfile.open(fileobj=io.BytesIO(archive),mode='r:') as tar:
        for x in tar.getmembers():
            p=Path(x.name)
            if p.is_absolute() or '..' in p.parts or x.name in seen or not (x.isdir() or x.isfile()): raise ValueError('unsafe historical archive')
            seen.add(x.name)
            if x.isfile():
                f=tar.extractfile(x)
                if f is None: raise ValueError('missing archive file')
                files[x.name]=h(f.read())
    launcher_paths=[p for p in files if p in paths[:4] or p.startswith(paths[4]+'/') or p.startswith(paths[5]+'/')]
    if not set(paths[:4])<=set(launcher_paths): raise ValueError('historical source incomplete')
    launcher=h(json.dumps([(p,files[p]) for p in sorted(launcher_paths,key=lambda p:Path(p).parts)],separators=(',',':')).encode())
    experiment=[(p[len(paths[6])+1:],files[p]) for p in sorted(files,key=lambda p:Path(p).parts) if p.startswith(paths[6]+'/')]
    if not experiment: raise ValueError('historical experiments missing')
    return {'launcher':launcher,'experiments':controls.digest(experiment)}


def historical_identity(commit,tree): return schema2.object_hash(historical_components(commit,tree))


def verify(doc, records):
    reviews=(launch.PROJECT/'reviews').resolve(strict=True); root=Path(doc['experiment_root']); rid=doc['reservation_id']; evidence=launch.jobs_root()/('planar-t044-'+rid); folder=root/'executions'/rid
    if root.is_symlink() or not root.is_dir() or evidence.is_symlink() or not evidence.is_dir() or folder.is_symlink() or not folder.is_dir(): raise ValueError('pretrial roots unavailable')
    if not root.resolve().is_relative_to(reviews) or not evidence.resolve().is_relative_to(reviews): raise ValueError('pretrial roots outside project reviews')
    m=schema2.validate(root,doc['experiment_identity']); reservation_path=root/'reservations'/(rid+'.json'); reservation=schema2.read_record(reservation_path); schema2_runnable.closed(reservation,schema2_runnable.RESERVATION_FIELDS)
    if reservation['reservation_id']!=rid or reservation['execution_id']!=rid or reservation['experiment_identity']!=doc['experiment_identity'] or reservation['account_scope_id']!=str(Path(records).parent.resolve()) or reservation['implementation_sha256']!=m['implementation_sha256'] or reservation['runnable_contract']!=schema2_runnable.CONTRACT: raise ValueError('reservation authority mismatch')
    selected=schema2.preflight(root,doc['experiment_identity'],reservation['logical_id']); variant=selected['variant']
    if m.get('runnable_contract')!=schema2_runnable.CONTRACT or len(m['trials'])!=1 or m['trials'][0]!=selected['trial'] or len(m['variants'])!=1:
        raise ValueError('unsupported experiment matrix')
    caps=m['configuration']['config']['caps']
    if caps.get('trials')!=1 or type(caps.get('trials')) is not int or caps.get('max_executions')!=1 or type(caps.get('max_executions')) is not int:
        raise ValueError('unsupported experiment cap')
    if any(reservation[k]!=variant[k] for k in (*schema2.IDENTITIES,'image_id','verifier_image_id','host_version','configuration_sha256')): raise ValueError('frozen variant mismatch')
    gate_path=safe_path(doc['gate_path'],reviews); gate_raw=read_safe(gate_path,reviews); gate=pv.parse(gate_raw,1024*1024); schema2_runnable.closed(gate,schema2_runnable.GATE_FIELDS)
    if h(gate_raw)!=doc['gate_sha256'] or type(gate['schema_version']) is not int or gate['schema_version']!=2 or type(gate['repetition']) is not int or gate['verdict']!='approve' or gate['purpose']!='one-experiment-trial' or type(gate['max_attempts']) is not int or gate['max_attempts']!=1 or gate['reservation_sha256']!=h(reservation_path.read_bytes()): raise ValueError('gate mismatch')
    for key in schema2.GATE_FIELDS & set(reservation):
        if key not in ('schema_version','expires_at') and canonical(gate[key])!=canonical(reservation[key]): raise ValueError('gate reservation mismatch')
    binding=reservation['concrete_binding']; schema2_runnable.closed(binding,schema2_runnable.BINDING_FIELDS)
    if binding['concrete_job_config_sha256']!=schema2_runnable.digest({k:v for k,v in binding.items() if k!='concrete_job_config_sha256'}) or gate['concrete_job_config_sha256']!=binding['concrete_job_config_sha256']: raise ValueError('concrete binding mismatch')
    if schema2.utc(gate['reviewed_at'])>schema2.utc(gate['expires_at']) or schema2.utc(gate['expires_at'])>schema2.utc(reservation['expires_at']) or schema2.utc(gate['expires_at'])>schema2.utc(m['deadline_at']): raise ValueError('historical gate timing mismatch')
    marker_hash=hashlib.sha256(json.dumps(gate,sort_keys=True,separators=(',',':')).encode()).hexdigest(); marker=launch.reviews_root()/'harbor-t044-consumed'/marker_hash
    if read_safe(marker,reviews)!=b'reserved\n': raise ValueError('gate consumption missing')
    shared=Path(records)/(rid+'.start.json'); shared_value=schema2.read_record(shared)
    if shared_value!={'experiment':str(root.resolve()),'execution_id':rid,'runnable_contract':schema2_runnable.CONTRACT,'reservation_sha256':h(reservation_path.read_bytes()),'gate_marker_sha256':marker_hash,'gate_raw_sha256':h(gate_raw)}: raise ValueError('shared start mismatch')
    local=schema2.read_record(folder/'start.json')
    expected_local={'concrete_job_config_sha256':binding['concrete_job_config_sha256'],'execution_id':rid,'gate_marker_sha256':marker_hash,'logical_id':reservation['logical_id'],'predecessor_execution_id':reservation['predecessor_execution_id'],'repetition':reservation['repetition'],'reservation_id':rid,'reservation_sha256':h(reservation_path.read_bytes()),'variant_id':reservation['variant_id']}
    if canonical(local)!=canonical(expected_local): raise ValueError('local start mismatch')
    dispatch=schema2.read_record(folder/'dispatch.json'); run_id='planar-t044-'+rid
    if dispatch!={'launcher_id':run_id,'evidence':str(evidence),'case':'native-lineage-portable','effort':binding['job_config']['agents'][0]['kwargs']['reasoning_effort'],'trial_deadline':min(binding['caps']['trial_seconds'],1500)}: raise ValueError('dispatch mismatch')
    expected_terminal={'status':'incomplete','reason':'portable native provenance or cleanup unconfirmed'}
    if schema2.read_record(folder/'terminal.json')!=expected_terminal or schema2.read_record(folder/'account-stop.json')!={'reason':expected_terminal['reason']}: raise ValueError('terminal or stop mismatch')
    result=schema2.read_record(folder/'launcher-result.json')
    if result!={'diagnostic_verdict':'pending','evidence':str(evidence),'execution_id':run_id,'return_code':1,'status':'completed'} or type(result['return_code']) is not int: raise ValueError('launcher result mismatch')
    allowed_exec=EXEC_FILES|({'account-stop-resolved.json'} if (folder/'account-stop-resolved.json').exists() else set())
    if {p.name for p in folder.iterdir()}!=allowed_exec: raise ValueError('unexpected execution artifact')
    if {p.name for p in evidence.iterdir()}!=EVIDENCE_FILES|{'projects'}: raise ValueError('unexpected pretrial artifact')
    registry=evidence/'projects'
    if registry.is_symlink() or not registry.is_dir() or any(registry.iterdir()): raise ValueError('guest registry not empty')
    def obj(name): return json.loads(read_safe(evidence/name,reviews),object_pairs_hook=__import__('terminal_failure_schema').unique)
    context=obj('schema2-context.json')
    if context!={'root':str(root),'identity':doc['experiment_identity'],'reservation_id':rid,'preparation_evidence':doc['preparation_path'],'gate_path':str(gate_path)}: raise ValueError('native context mismatch')
    config_raw=read_safe(evidence/'job-config.json',reviews); json.loads(config_raw,object_pairs_hook=__import__('terminal_failure_schema').unique)
    config=JobConfig.model_validate_json(config_raw)
    if not schema2_projection.exact(schema2_projection.project(config),binding['job_config']): raise ValueError('submitted configuration mismatch')
    full,normalized=schema2_projection.normalize(config,execution_id=rid,evidence_raw=str(evidence),case_raw=selected['instance'],effort=variant['effort'])
    policy=m['configuration']['config']['native_config']
    if not schema2_projection.exact(full,binding['job_config']) or not schema2_projection.exact(normalized,policy['normalized_job_config']): raise ValueError('native policy mismatch')
    from harbor.models.task.task import Task
    verifier=Task(Path(selected['instance'])).config.verifier.environment
    effective=schema2_projection.effective_verifier_environment(verifier,reservation['verifier_image_id'])
    if not schema2_projection.exact(schema2_projection.project(effective),binding['effective_verifier_environment']): raise ValueError('verifier environment mismatch')
    launcher=obj('launcher.json')
    if launcher.get('execution_id')!=run_id or launcher.get('case')!='native-lineage-portable' or launcher.get('implementation_sha256')!=historical_components(doc['historical_commit'],doc['historical_tree'])['launcher'] or launcher.get('image_id')!=reservation['image_id'] or launcher.get('verifier_image_id')!=reservation['verifier_image_id'] or launcher.get('review_sha256')!=doc['gate_sha256'] or launcher.get('job_config_sha256')!=h(config_raw): raise ValueError('launcher authority mismatch')
    snapshot=launch.plan('native-lineage-portable',candidate_root=Path(selected['candidate']),case_root=Path(selected['instance']),trial_deadline=dispatch['trial_deadline'],effort=dispatch['effort'])
    if any(launcher.get(k)!=snapshot[k] for k in ('case_sha256','candidate_sha256','configuration_sha256')): raise ValueError('launcher input binding mismatch')
    if launcher.get('runnable_contract')!=schema2_runnable.CONTRACT or not schema2_projection.exact(launcher.get('effective_verifier_environment'),binding['effective_verifier_environment']): raise ValueError('launcher verifier environment mismatch')
    command=obj('launcher-command.json'); argv=command.get('argv')
    keys={'CODEX_API_KEY','CODEX_AUTH_JSON_PATH','HOME','OPENAI_API_KEY','PATH','PLANAR_EVALS_OUTPUT_ROOT','PLANAR_SCHEMA2_CONTEXT','PLANAR_T044_CANDIDATE_SNAPSHOT','PLANAR_T044_CASE','PLANAR_T044_CASE_SHA256','PLANAR_T044_CASE_SNAPSHOT','PLANAR_T044_DEADLINE_SECONDS','PLANAR_T044_EFFORT','PLANAR_T044_EVIDENCE_DIR','PLANAR_T044_EXPECTED_IMAGE_ID','PLANAR_T045_VERIFIER_IMAGE_ID','PYTHONPATH'}
    if set(command)!={'execution_id','argv','environment_keys'} or command.get('execution_id')!=run_id or not isinstance(argv,list) or len(argv)!=4 or argv[1]!=str(launch.ROOT/'evals/planar/adapters/native_job.py') or argv[0]!=str(launch.EVALS/'.venv/bin/python') or argv[2:]!=[str(evidence/'job-config.json'),str(evidence)] or not isinstance(command.get('environment_keys'),list) or set(command['environment_keys'])!=keys or len(command['environment_keys'])!=len(keys): raise ValueError('native command mismatch')
    process=obj('harbor-process.json')
    if set(process)!={'execution_id','pid'} or process.get('execution_id')!=run_id or type(process.get('pid')) is not int or process['pid']<=1: raise ValueError('native process mismatch')
    for name in ('launcher-terminal.json','watchdog-terminal.json'):
        terminal=obj(name)
        fields={'execution_id','status','return_code'}|({'finished_at'} if name=='launcher-terminal.json' else set())
        if set(terminal)!=fields or terminal.get('execution_id')!=run_id or terminal.get('status')!='completed' or type(terminal.get('return_code')) is not int or terminal['return_code']!=1: raise ValueError('pretrial supervisor mismatch')
    prep=Path(doc['preparation_path']); prep_raw=read_safe(prep,reviews); prep_doc=pv.parse(prep_raw,1024*1024)
    if h(prep_raw)!=binding['verifier_preparation']['preparation_evidence_sha256'] or h(read_safe(prep.with_name(prep.name+'.build.log'),reviews))!=prep_doc['build_log_sha256']: raise ValueError('preparation mismatch')
    audit=Path(doc['independent_audit_path']);
    if h(read_safe(audit,reviews))!=doc['independent_audit_sha256']: raise ValueError('independent audit mismatch')
    if historical_identity(doc['historical_commit'],doc['historical_tree'])!=doc['historical_implementation_sha256'] or doc['historical_implementation_sha256']!=m['implementation_sha256']: raise ValueError('historical source mismatch')
    resolver_tree=subprocess.run(['git','rev-parse',doc['resolver_commit']+'^{tree}'],cwd=launch.ROOT,check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    if historical_identity(doc['resolver_commit'],resolver_tree)!=doc['resolver_implementation_sha256']: raise ValueError('resolver source mismatch')
    required={str(p) for p in [root/'manifest.json',reservation_path,gate_path,marker,shared,prep,prep.with_name(prep.name+'.build.log'),audit,*[folder/x for x in EXEC_FILES],*[evidence/x for x in EVIDENCE_FILES]]}
    if set(doc['artifacts'])!=required: raise ValueError('authority inventory mismatch')
    for path,expected in doc['artifacts'].items():
        if h(read_safe(path,reviews))!=expected: raise ValueError('authority artifact changed')
    return {'root':root,'evidence':evidence,'folder':folder,'process_pid':process['pid'],'run_id':run_id}


def live_check(verified):
    evidence=verified['evidence']; run_id=verified['run_id']; pid=verified['process_pid']
    if (evidence/'recovery-required').exists() or (evidence/'recovery-required').is_symlink(): raise ValueError('recovery pending')
    try: os.killpg(pid,0)
    except ProcessLookupError: pass
    except PermissionError: raise ValueError('native group state unknown') from None
    else: raise ValueError('native group active')
    for kind,args in (('container',['ls','-aq']),('network',['ls','-q']),('volume',['ls','-q'])):
        result=subprocess.run(['docker',kind,*args,'--filter','name='+run_id],capture_output=True,text=True,timeout=15)
        if result.returncode or result.stdout.strip(): raise ValueError('owned resource absence unproven')
    census=retained_b_census.check(launch.ROOT,run_id,evidence)
    if type(census.get('matching_commands')) is not int or census['matching_commands']!=0 or type(census.get('unresolved')) is not int or census['unresolved']!=0: raise ValueError('command census unresolved')
    from datetime import datetime,timezone
    return {**census,'observed_at':datetime.now(timezone.utc).isoformat(),'execution_id':run_id,'native_group':'absent','resource_query':'exact execution prefix: zero matches'}


def resolver_identity():
    def git(*args): return subprocess.run(['git',*args],cwd=launch.ROOT,check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    commit=git('rev-parse','HEAD')
    if git('status','--porcelain','--untracked-files=no') or git('ls-files','--others','--exclude-standard','--','evals/planar/adapters','evals/planar/experiments','evals/planar/cases/plugin-smoke'):
        raise ValueError('resolver checkout not clean')
    git('ls-files','--error-unmatch','evals/planar/experiments/schema2_pretrial_release.py')
    return {'commit':commit,'implementation_sha256':schema2_execution.implementation_identity()}


def _check_pair(start, resolved, *, locked=False):
    """Both marker files and fresh evidence are required; caller holds shared lock."""
    import codex_run
    start=Path(start); resolved=Path(resolved); records=start.parent
    if codex_run._ACTIVE_ADMISSION.get()!=records.resolve(): return False
    try:
        reviews=(launch.PROJECT/'reviews').resolve(strict=True)
        marker=pv.parse(read_safe(resolved,reviews),4*1024*1024)
        fields={'schema','type','reservation_id','review_path','review_sha256','review_binding','census_path','census_sha256'}
        if not isinstance(marker,dict) or set(marker)!=fields or marker['schema']!='planar-schema2-pretrial-marker-v1' or marker['type']!='schema2-pretrial-v1': return False
        rid=marker['reservation_id'];
        if resolved!=records/(rid+'.resolved.json') or start!=records/(rid+'.start.json'): return False
        review=safe_path(marker['review_path'],reviews); raw=read_safe(review,reviews); doc=parse(raw)
        if h(raw)!=marker['review_sha256'] or canonical(doc)!=canonical(marker['review_binding']) or doc['reservation_id']!=rid: return False
        root=Path(doc['experiment_root']); local=root/'executions'/rid/'account-stop-resolved.json'
        if not locked:
            with schema2_execution._controller_lock(root): return _check_pair(start,resolved,locked=True)
        if read_safe(local,reviews)!=canonical(marker): return False
        census_path=Path(marker['census_path'])
        if census_path.parent!=records.resolve() or census_path.name!=rid+'.pretrial-census-'+marker['census_sha256']+'.json': return False
        census_raw=read_safe(census_path,reviews); census=pv.parse(census_raw,4*1024*1024)
        if h(census_raw)!=marker['census_sha256'] or census.get('execution_id')!='planar-t044-'+rid or census.get('native_group')!='absent' or type(census.get('matching_commands')) is not int or census['matching_commands']!=0 or type(census.get('unresolved')) is not int or census['unresolved']!=0: return False
        verified=verify(doc,records); live_check(verified)
        if read_safe(review,reviews)!=raw or verify(doc,records)!=verified: return False
        return True
    except (OSError,ValueError,TypeError,KeyError,AttributeError,subprocess.SubprocessError): return False


def validate_published(start,resolved): return _check_pair(start,resolved)


def publish(review):
    """Explicit independently reviewed account-only resolution; never runs a trial."""
    import codex_run
    reviews=(launch.PROJECT/'reviews').resolve(strict=True); review=safe_path(review,reviews); raw=read_safe(review,reviews); doc=parse(raw); rid=doc['reservation_id']; root=Path(doc['experiment_root'])
    with codex_run.admission_scope() as records, schema2_execution._controller_lock(root):
        if read_safe(review,reviews)!=raw: raise ValueError('review changed before locked verification')
        shared=records/(rid+'.resolved.json'); local=root/'executions'/rid/'account-stop-resolved.json'; closed=records/(rid+'.closed.json')
        if closed.exists() or closed.is_symlink(): raise ValueError('conflicting closed record')
        verified=verify(doc,records)
        if resolver_identity()!={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}: raise ValueError('resolver identity mismatch')
        census=live_check(verified); census_raw=canonical(census); census_sha=h(census_raw); census_path=records/(rid+'.pretrial-census-'+census_sha+'.json')
        if read_safe(review,reviews)!=raw: raise ValueError('review changed before publication')
        if verify(doc,records)!=verified or resolver_identity()!={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}: raise ValueError('authority changed before publication')
        marker={'schema':'planar-schema2-pretrial-marker-v1','type':'schema2-pretrial-v1','reservation_id':rid,'review_path':str(review),'review_sha256':h(raw),'review_binding':doc,'census_path':str(census_path),'census_sha256':census_sha}
        has_shared=shared.exists() or shared.is_symlink(); has_local=local.exists() or local.is_symlink()
        if has_shared or has_local:
            if not (has_shared and has_local): raise ValueError('partial resolution requires independent recovery')
            previous=pv.parse(read_safe(shared,reviews),4*1024*1024)
            if any(canonical(previous[k])!=canonical(marker[k]) for k in marker if k not in ('census_path','census_sha256')) or not _check_pair(records/(rid+'.start.json'),shared,locked=True): raise ValueError('conflicting resolution')
            return {'execution_id':rid,'account':'resolved','outcome':'incomplete'}
        try:
            fd=os.open(census_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        except FileExistsError:
            if read_safe(census_path,reviews)!=census_raw: raise ValueError('conflicting census') from None
        else:
            with os.fdopen(fd,'wb') as f: f.write(census_raw); f.flush(); os.fsync(f.fileno())
        schema2_execution._sync_directory(records)
        # Local first: neither a lone local marker nor a lone shared marker can release.
        schema2.write_new(local,marker)
        schema2.write_new(shared,marker)
        return {'execution_id':rid,'account':'resolved','outcome':'incomplete'}
