"""Portable schema-2 freeze, reservation and review boundary (TS-25.8).

This module does not launch, authorize, consume gates, inspect images, or
activate an account slot. The guarded adapter must perform those operations.
"""
from datetime import datetime, timedelta, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
from uuid import uuid4

import per_variant as pv

IDENTITIES = ('candidate_inventory_sha256', 'candidate_snapshot_sha256',
              'case_template_sha256', 'case_instance_sha256',
              'verifier_code_sha256', 'trusted_inputs_sha256')
GATE_FIELDS = frozenset(('schema_version', 'purpose', 'verdict', 'max_attempts',
    'experiment_identity', 'logical_id', 'variant_id', 'repetition',
    'reservation_id', 'reservation_sha256', 'execution_id',
    'predecessor_execution_id', 'retry_reason_sha256', *IDENTITIES,
    'configuration_sha256', 'implementation_sha256', 'image_id',
    'verifier_image_id', 'host_version', 'account_scope_id',
    'canonical_output_root', 'expires_at', 'reviewer', 'report', 'reviewed_at'))
SKILLS = ('check', 'draft', 'gap', 'project', 'replicate', 'review', 'run')

class Invalid(ValueError): pass

def h(data): return hashlib.sha256(data).hexdigest()
def object_hash(obj): return h(pv.canonical(obj))

def utc(value):
    try:
        dt = datetime.fromisoformat(value.replace('Z', '+00:00'))
        if dt.utcoffset() != timezone.utc.utcoffset(dt): raise ValueError()
        return dt
    except (ValueError, TypeError, AttributeError): raise Invalid('timezone-aware UTC required') from None

def write_new(path, value):
    data = pv.canonical(value)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(data); f.flush(); os.fsync(f.fileno())
    fd = os.open(Path(path).parent, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)

def read_record(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file(): raise Invalid('missing or unsafe record')
    return pv.parse(path.read_bytes(), 4*1024*1024)

def image(value):
    if not isinstance(value, str) or not value.startswith('sha256:') or not pv.HEX.fullmatch(value[7:]):
        raise Invalid('resolved image identity required')
    return value

def package(root):
    root = Path(root)
    pv.tree(root)
    try: meta = json.loads((root/'.claude-plugin/plugin.json').read_bytes())
    except (OSError, ValueError): raise Invalid('missing package metadata') from None
    if not isinstance(meta, dict) or meta.get('name') != 'planar' or meta.get('version') != '0.1.0':
        raise Invalid('unsupported package contract')
    if any(not (root/'skills'/name/'SKILL.md').is_file() for name in SKILLS):
        raise Invalid('missing core skill')

def initialize(root, *, template, reviewed_template_sha256, variants, repetitions,
               config, implementation_sha256, deadline_at, account_scope_id, runnable_contract=None):
    """Freeze explicit local inputs. Image IDs are supplied observations, not verified here."""
    root, template = Path(root), Path(template)
    if root.exists() or root.is_symlink(): raise Invalid('experiment root exists')
    if not isinstance(variants, list) or not variants or type(repetitions) is not int or not 1 <= repetitions <= 100:
        raise Invalid('bounded matrix required')
    if not isinstance(config, dict) or set(config) != {'model','variant_efforts','caps','native_config'} or config['model'] != 'gpt-6.1-terra' or not isinstance(config['native_config'],dict) or not config['native_config'] or not isinstance(account_scope_id, str) or not account_scope_id:
        raise Invalid('closed configuration and scope required')
    caps = config['caps']
    if not isinstance(caps,dict) or set(caps) != {'session_seconds','verifier_seconds','trial_seconds','job_seconds','trials','max_executions'} or any(type(v) is not int or not 0 < v <= 86400 for v in caps.values()):
        raise Invalid('finite caps required')
    if not isinstance(implementation_sha256, str) or not pv.HEX.fullmatch(implementation_sha256): raise Invalid('implementation identity required')
    now = datetime.now(timezone.utc); deadline = utc(deadline_at)
    if deadline <= now or deadline > now + timedelta(seconds=caps['job_seconds']): raise Invalid('deadline exceeds finite job wall-time cap')
    if caps['trial_seconds'] > caps['job_seconds'] or caps['session_seconds'] > caps['trial_seconds'] or caps['verifier_seconds'] > caps['trial_seconds']:
        raise Invalid('inconsistent time caps')
    pv.validate_template(template, reviewed_template_sha256)
    ids = []
    for v in variants:
        if not isinstance(v, dict) or set(v) != {'id','candidate_source','effort','image_id','verifier_image_id','host_version'}:
            raise Invalid('closed variant required')
        pv.safe_id(v['id']); ids.append(v['id'])
        if v['effort'] not in ('medium','high') or not isinstance(v['host_version'], str) or not v['host_version']:
            raise Invalid('unsupported effort or host')
        image(v['image_id']); image(v['verifier_image_id']); package(v['candidate_source'])
    if len(ids) != len(set(ids)): raise Invalid('duplicate variant')
    if config['variant_efforts'] != {v['id']:v['effort'] for v in variants} or len(variants)*repetitions > caps['trials'] or len(variants)*repetitions > caps['max_executions']:
        raise Invalid('configuration effort or trial cap mismatch')
    if runnable_contract is not None:
        if runnable_contract != 'planar-schema2-runnable-v1': raise Invalid('unsupported runnable contract')
        # Use the actual guarded launcher and default-resolved pinned JobConfig;
        # only a policy matching every field and concrete scope can be frozen.
        import sys
        sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
        import launch
        from harbor.models.job.config import JobConfig
        from harbor.models.task.task import Task
        import schema2_runnable
        policy = config['native_config']
        schema2_runnable.closed(policy,schema2_runnable.POLICY_FIELDS)
        schema2_runnable.closed(policy['roots'],schema2_runnable.ROOTS)
        expected_roots = {'experiment':root.absolute(), 'reviews':launch.reviews_root().absolute(),
                          'jobs':launch.jobs_root().absolute(), 'consumed':(launch.reviews_root()/'harbor-t044-consumed').absolute()}
        for key, raw in expected_roots.items():
            if policy['roots'][key] != {'raw':str(raw),'resolved':str(raw.resolve())}:
                raise Invalid('runnable output scope mismatch')
        if account_scope_id != str((launch.reviews_root()/'planar-account-admission-v1').resolve()):
            raise Invalid('runnable account scope mismatch')
        task = Task(template).config
        if task.environment.env or task.verifier.env or task.verifier.environment is None or task.verifier.environment.env or task.verifier.environment_mode.value != 'separate': raise Invalid('unsupported frozen task channels')
        if task.agent.timeout_sec > caps['session_seconds'] or task.verifier.timeout_sec > caps['verifier_seconds']: raise Invalid('case exceeds reviewed caps')
        trial_id = '0'*32
        evidence = expected_roots['jobs']/('planar-t044-'+trial_id)
        for v in variants:
            if v['host_version'] != 'codex-cli 0.169.0' or v['image_id'] != task.environment.docker_image: raise Invalid('unsupported runnable host or task image')
            schema2_runnable.projection.effective_verifier_environment(task.verifier.environment,v['verifier_image_id'])
            case = root.absolute()/'variants'/v['id']/'case-instance'
            job = JobConfig.model_validate(launch.config_for('planar-t044-'+trial_id,evidence,case,v['effort']))
            expected_policy = schema2_runnable.derive_policy(job, execution_id=trial_id,
                evidence_raw=str(evidence),case_raw=str(case),effort=v['effort'],roots=policy['roots'])
            if not schema2_runnable.projection.exact(policy,expected_policy): raise Invalid('runnable JobConfig policy mismatch')
    root.mkdir(mode=0o700)
    # Failure leaves a non-reusable partial root. It is never silently repaired.
    (root/'variants').mkdir(mode=0o700); (root/'reservations').mkdir(mode=0o700); (root/'executions').mkdir(mode=0o700)
    shutil.copytree(template, root/'case-template', symlinks=True)
    pv.validate_template(root/'case-template', reviewed_template_sha256)
    records = []
    for v in variants:
        source = Path(v['candidate_source'])
        before = pv.tree_hash(source)
        folder = root/'variants'/v['id']; folder.mkdir(mode=0o700)
        shutil.copytree(source, folder/'candidate', symlinks=True)
        if pv.tree_hash(source) != before or pv.tree_hash(folder/'candidate') != before: raise Invalid('candidate changed during freeze')
        package(folder/'candidate')
        execution = {'schema_version':1,'driver':{'model':'gpt-6.1-terra','effort':v['effort']},'child':{'model':'gpt-6.1-terra','effort':'medium'}}
        identities = pv.materialize(root/'case-template', folder/'case-instance', folder/'candidate', execution, expected_template_sha256=reviewed_template_sha256)
        identities['candidate_snapshot_sha256'] = before
        records.append({'variant_id':v['id'], 'source':str(source.absolute()), 'resolved_source':str(source.resolve()), 'effort':v['effort'], 'image_id':v['image_id'], 'verifier_image_id':v['verifier_image_id'], 'host_version':v['host_version'], **identities})
    # The complete provided configuration, resolved root and caps must be supplied
    # and compared to actual launcher configuration at the integration boundary.
    configuration = {'config':config, 'raw_output_root':str(root.absolute()), 'canonical_output_root':str(root.resolve()), 'account_scope_id':account_scope_id}
    for v in records:
        v['configuration'] = {**configuration, 'task':{'case':'planar/native-lineage-portable','contract_version':2},
            'variant_id':v['variant_id'], 'effort':v['effort'], 'image_id':v['image_id'],
            'verifier_image_id':v['verifier_image_id'], 'host_version':v['host_version']}
        v['configuration_sha256'] = object_hash(v['configuration'])
    manifest = {'schema_version':2, 'case':'planar/native-lineage-portable', 'contract_version':2,
        'configuration':configuration, 'configuration_sha256':object_hash(configuration),
        'implementation_sha256':implementation_sha256, 'deadline_at':deadline_at,
        'variants':records, 'trials':[{'logical_id':f"{v['variant_id']}:{n}", 'variant_id':v['variant_id'], 'repetition':n} for n in range(1,repetitions+1) for v in records]}
    if runnable_contract is not None: manifest['runnable_contract'] = runnable_contract
    manifest['identity'] = object_hash(manifest)
    write_new(root/'manifest.json', manifest)
    return manifest

def validate(root, identity):
    root = Path(root)
    if root.is_symlink() or not root.is_dir(): raise Invalid('unsafe experiment root')
    for name in ('variants','case-template','reservations','executions'):
        path = root/name
        if path.is_symlink() or not path.is_dir(): raise Invalid('unsafe control path')
    m = read_record(root/'manifest.json')
    if not isinstance(m, dict) or m.get('schema_version') != 2 or m.get('identity') != identity or object_hash({k:v for k,v in m.items() if k != 'identity'}) != identity:
        raise Invalid('manifest identity mismatch')
    if str(root.absolute()) != m['configuration']['raw_output_root'] or str(root.resolve()) != m['configuration']['canonical_output_root'] or object_hash(m['configuration']) != m['configuration_sha256']:
        raise Invalid('output or configuration drift')
    for v in m['variants']:
        pv.safe_id(v['variant_id'])
        folder = root/'variants'/v['variant_id']
        if folder.is_symlink() or not folder.is_dir(): raise Invalid('unsafe variant root')
        for path in (folder/'candidate', folder/'case-instance'):
            if path.is_symlink() or not path.is_dir(): raise Invalid('unsafe selected root')
        expected_config = {**m['configuration'], 'task':{'case':'planar/native-lineage-portable','contract_version':2},
            'variant_id':v['variant_id'], 'effort':v['effort'], 'image_id':v['image_id'], 'verifier_image_id':v['verifier_image_id'], 'host_version':v['host_version']}
        if v['configuration'] != expected_config or v['configuration_sha256'] != object_hash(expected_config): raise Invalid('variant configuration drift')
        package(folder/'candidate')
        checked = pv.validate_instance(root/'case-template', folder/'case-instance', folder/'candidate', expected_template_sha256=v['case_template_sha256'])
        checked['candidate_snapshot_sha256'] = pv.tree_hash(folder/'candidate')
        if any(v[k] != checked[k] for k in IDENTITIES): raise Invalid('frozen input drift')
        execution = pv.validate_execution((folder/'case-instance/execution.json').read_bytes())
        if execution['driver']['effort'] != v['effort']: raise Invalid('effort drift')
    return m

def preflight(root, identity, logical_id):
    m = validate(root, identity)
    found = [t for t in m['trials'] if t['logical_id'] == logical_id]
    if len(found) != 1: raise Invalid('unknown logical trial')
    trial = found[0]; v = next(v for v in m['variants'] if v['variant_id'] == trial['variant_id'])
    folder = Path(root)/'variants'/v['variant_id']
    return {'trial':trial, 'variant':v, 'candidate':str(folder/'candidate'), 'instance':str(folder/'case-instance'), 'manifest':m}

def reserve(root, identity, logical_id, *, expires_at, predecessor_execution_id=None, retry_reason=None):
    root = Path(root)
    if root.is_symlink() or not root.is_dir(): raise Invalid('unsafe experiment root')
    try:
        fd = os.open(root/'reservation.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    except OSError as exc: raise Invalid('unsafe reservation lock') from exc
    with os.fdopen(fd, 'r+') as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode): raise Invalid('unsafe reservation lock')
        fcntl.flock(lock, fcntl.LOCK_EX)
        selected = preflight(root, identity, logical_id); m, v, trial = selected['manifest'], selected['variant'], selected['trial']
        if m.get('runnable_contract') is not None: raise Invalid('runnable manifest requires runnable reservation route')
        expiry = utc(expires_at)
        if expiry <= datetime.now(timezone.utc) or expiry > utc(m['deadline_at']): raise Invalid('reservation expiry')
        existing = [read_record(p) for p in (root/'reservations').glob('*.json')]
        attempts = [r for r in existing if r['logical_id'] == logical_id]
        if attempts:
            leaves = [r for r in attempts if r['execution_id'] not in {x['predecessor_execution_id'] for x in attempts}]
            if len(leaves) != 1 or predecessor_execution_id != leaves[0]['execution_id'] or not isinstance(retry_reason,str) or not retry_reason.strip(): raise Invalid('explicit latest retry required')
            terminal = read_record(root/'executions'/predecessor_execution_id/'terminal.json')
            if terminal.get('status') not in ('incomplete','error'): raise Invalid('predecessor not retryable')
        elif predecessor_execution_id is not None or retry_reason is not None:
            raise Invalid('unexpected primary retry fields')
        # Until budget is mapped to the existing scheduler, a caller must provide
        # an explicit finite maximum, including retries, in configuration.
        limit = m['configuration']['config']['caps']['max_executions']
        if type(limit) is not int or not 0 < limit <= 1000 or len(existing) >= limit: raise Invalid('finite execution budget exhausted or missing')
        eid = uuid4().hex
        record = {'schema_version':2, 'state':'planned', 'reservation_id':eid, 'execution_id':eid, 'experiment_identity':identity, **trial,
            'predecessor_execution_id':predecessor_execution_id, 'retry_reason_sha256':h(retry_reason.encode()) if retry_reason else None,
            **{key:v[key] for key in IDENTITIES}, 'configuration_sha256':v['configuration_sha256'], 'implementation_sha256':m['implementation_sha256'],
            'image_id':v['image_id'], 'verifier_image_id':v['verifier_image_id'], 'host_version':v['host_version'],
            'account_scope_id':m['configuration']['account_scope_id'], 'canonical_output_root':m['configuration']['canonical_output_root'], 'expires_at':expires_at}
        write_new(root/'reservations'/f'{eid}.json', record)
        return record

def validate_gate(root, identity, reservation_id, gate, *, now=None, allow_activated=False):
    """Review-only comparison; adapter must recheck and atomically consume before auth."""
    pv.safe_id(reservation_id)
    root = Path(root); r = read_record(root/'reservations'/f'{reservation_id}.json')
    selected = preflight(root, identity, r['logical_id']); m = selected['manifest']
    if r['reservation_id'] != reservation_id or r['execution_id'] != reservation_id or r['experiment_identity'] != identity: raise Invalid('foreign reservation')
    if (root/'executions'/reservation_id).exists() and not allow_activated: raise Invalid('reservation already activated')
    if not isinstance(gate, dict) or set(gate) != GATE_FIELDS or type(gate.get('schema_version')) is not int or type(gate.get('max_attempts')) is not int or type(gate.get('repetition')) is not int: raise Invalid('gate fields')
    expected = {k:r[k] for k in GATE_FIELDS if k in r and k not in ('schema_version','expires_at')}
    expected.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':h((root/'reservations'/f'{reservation_id}.json').read_bytes())})
    if any(gate.get(k) != v for k,v in expected.items()): raise Invalid('gate mismatch')
    if any(not isinstance(gate[k],str) or not gate[k].strip() for k in ('reviewer','report','reviewed_at')): raise Invalid('review metadata')
    reviewed = utc(gate['reviewed_at']); expiry = utc(gate['expires_at']); current = now or datetime.now(timezone.utc)
    if reviewed > current or expiry <= current or expiry > utc(r['expires_at']) or expiry > utc(m['deadline_at']): raise Invalid('gate expiry or review time')
    # Reservation must still bind selected frozen variant, not merely its own gate.
    v = selected['variant']
    if any(r[k] != selected['trial'][k] for k in ('logical_id','variant_id','repetition')) or r['account_scope_id'] != m['configuration']['account_scope_id'] or r['canonical_output_root'] != m['configuration']['canonical_output_root'] or r['state'] != 'planned': raise Invalid('reservation scope drift')
    if any(r[k] != v[k] for k in (*IDENTITIES,'image_id','verifier_image_id','host_version')) or r['configuration_sha256'] != v['configuration_sha256'] or r['implementation_sha256'] != m['implementation_sha256']: raise Invalid('reservation drift')
    return selected
