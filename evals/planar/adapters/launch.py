"""Bounded Harbor plan and review-gated diagnostic launcher for T-044."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
import re
import runpy
import importlib.metadata
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import uuid

from harbor.models.job.config import JobConfig
from harbor.models.task.task import Task
from candidate import inventory
from smoke_codex import CANDIDATE, CONFIG, MANIFEST, ROOT, HOME, CACHE, validate_recovery_selection
from supervise import supervise
from recover import recover_projects, jobs_root, output_roots

PROJECT = ROOT.parent
REVIEWS = PROJECT / 'reviews'
EVALS = ROOT / 'evals'
CASE = EVALS / 'planar/cases/plugin-smoke'
CASES = {name: EVALS / 'planar/cases' / name for name in ('plugin-smoke', 'native-lineage-smoke', 'native-lineage-current', 'fresh-interrupted-review', 'clean-revision', 'retained-a-fresh-b')}


def case_inventory(case):
    entries = []
    for path in sorted(case.rglob('*')):
        if '__pycache__' in path.parts:
            continue
        if path.is_symlink():
            raise ValueError('symlink in case')
        if path.is_file():
            entries.append({'path': path.relative_to(case).as_posix(), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    if not entries:
        raise ValueError('empty case')
    return hashlib.sha256(json.dumps(entries, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def source_digest(case=CASE):
    paths = [EVALS / 'pyproject.toml', EVALS / 'uv.lock', EVALS / 'README.md', EVALS / 'planar/README.md']
    for directory in (EVALS / 'planar/adapters', case):
        for path in directory.rglob('*'):
            if '__pycache__' in path.parts:
                continue
            if path.is_symlink():
                raise ValueError('symlink in evaluation source')
            if path.is_file():
                paths.append(path)
    if Path(case).name == 'retained-a-fresh-b':
        paths.append(EVALS/'planar/experiments/codex_run.py')
    records = []
    for path in sorted(paths):
        if path.is_symlink():
            raise ValueError('symlink in evaluation source')
        records.append((path.relative_to(ROOT).as_posix(), hashlib.sha256(path.read_bytes()).hexdigest()))
    return hashlib.sha256(json.dumps(records, separators=(',', ':')).encode()).hexdigest()


def config_for(job_name, jobs_dir, case=CASE, effort='medium'):
    data = {'job_name': job_name, 'jobs_dir': str(jobs_dir), 'n_attempts': 1,
            'n_concurrent_trials': 1, 'retry': {'max_retries': 0},
            'environment': {'import_path': 'guarded_docker:GuardedDocker', 'delete': True, 'kwargs': {'auto_mounting': False}},
            'agents': [{'import_path': 'smoke_codex:SmokeCodex', 'model_name': 'gpt-6.1-terra',
                        'kwargs': {'version': '0.169.0', 'config': CONFIG, 'reasoning_effort': effort}}],
            'tasks': [{'path': str(case)}]}
    return JobConfig.model_validate(data).model_dump(mode='json', exclude_none=True)


def validate_write_destination(path):
    """Check an existing directory or its nearest existing ancestor without creating it."""
    current = path
    while not current.exists() and not current.is_symlink():
        current = current.parent
    if current.is_symlink() or not current.is_dir():
        raise ValueError(f'write destination or ancestor is not a directory: {current}')
    if not os.access(current, os.W_OK | os.X_OK):
        raise ValueError(f'write destination or ancestor is not writable: {current}')
    return {'path': str(path), 'checked_ancestor': str(current),
            'status': 'existing directory checked' if current == path else 'ancestor checked; creation pending'}


def reviews_root():
    return output_roots()[0] if os.environ.get('PLANAR_EVALS_OUTPUT_ROOT') else REVIEWS


def plan(case_name='plugin-smoke', *, candidate_root=None, case_root=None, trial_deadline=1500, effort='medium', source_packet=None, execution_id=None, absolute_deadline=None):
    retained = case_name == 'retained-a-fresh-b'
    if not retained and source_packet is not None: raise ValueError('source packet is retained-case only')
    if retained and (trial_deadline != 1050 or effort != 'medium' or not candidate_root or not case_root or not source_packet or not execution_id or not re.fullmatch(r'planar-t044-[0-9a-f]{32}',execution_id)):
        raise ValueError('retained case requires frozen inputs, reserved execution and fixed budgets')
    if retained:
        try:
            expiry=datetime.fromisoformat(absolute_deadline)
            if expiry.tzinfo is None or expiry <= datetime.now(timezone.utc): raise ValueError()
        except (ValueError,TypeError): raise ValueError('retained case requires finite future expiry') from None
    if case_name == 'clean-revision' and (trial_deadline != 1050 or effort != 'medium'):
        raise ValueError('clean revision requires the reviewed 1050-second job and medium effort')
    if case_name == 'clean-revision' and (candidate_root is None or case_root is None):
        raise ValueError('clean revision requires paired frozen candidate and case snapshots')
    if effort not in ('medium', 'high') or (case_name == 'fresh-interrupted-review' and effort != 'medium'):
        raise ValueError('unsupported effort for selected case')
    if not isinstance(trial_deadline, int) or not 1 <= trial_deadline <= 1500:
        raise ValueError('trial deadline must be between 1 and 1500 seconds')
    if (candidate_root is None) != (case_root is None):
        raise ValueError('candidate and case snapshots must be supplied together')
    candidate = Path(candidate_root).resolve(strict=True) if candidate_root is not None else CANDIDATE
    case = Path(case_root).resolve(strict=True) if case_root is not None else CASES[case_name]
    if Path.cwd().resolve() != ROOT.resolve():
        raise ValueError('launch must run from the repository root')
    for label, path in (('repository', ROOT), ('package', candidate), ('case', case), ('project', ROOT if os.environ.get('PLANAR_EVALS_OUTPUT_ROOT') else PROJECT),):
        if not path.is_dir():
            raise ValueError(f'{label} root is unavailable: {path}')
    if not os.environ.get('PLANAR_EVALS_OUTPUT_ROOT') and not REVIEWS.is_dir():
        raise ValueError(f'evidence root is unavailable: {REVIEWS}')
    git_marker = ROOT / '.git'
    if git_marker.is_dir():
        git_dir = git_marker
    elif git_marker.is_file() and git_marker.read_text().startswith('gitdir: '):
        git_dir = (ROOT / git_marker.read_text().strip()[8:]).resolve()
        if not (git_dir / 'gitdir').is_file() or Path((git_dir / 'gitdir').read_text().strip()).resolve() != git_marker.resolve():
            raise ValueError('repository git worktree identity is invalid')
    else:
        raise ValueError('repository git root is invalid')
    if not (git_dir / 'HEAD').is_file() or not (candidate / '.claude-plugin/plugin.json').is_file():
        raise ValueError('repository or package root is invalid')
    write_roots = {'harbor-t044-jobs': validate_write_destination(jobs_root()),
                   'harbor-t044-consumed': validate_write_destination(reviews_root() / 'harbor-t044-consumed')}
    if retained:
        from retained_b_support import source as validate_retained_source
        source_identity=validate_retained_source(case,source_packet,candidate)
        if source_identity.get('assessment',{}).get('status') != 'pass': raise ValueError('retained source assessment failed')
    expected = json.loads((case / 'candidate.json').read_text())
    if inventory(candidate) != expected:
        raise ValueError('candidate differs from recorded frozen inventory')
    config = config_for('planar-t044-plan', jobs_root(), case, effort)
    if case_name == 'fresh-interrupted-review':
        selected = config['agents'][0]
        validate_recovery_selection((case / 'instruction.md').read_text(), selected['model_name'], selected['kwargs']['reasoning_effort'])
        compatibility = runpy.run_path(str(case / 'compatibility.py'))['inspect'](candidate, case / 'fixture-v2')
        if compatibility['missing_packet_sections'] or compatibility['immutable_replacement_required']:
            raise ValueError('v2 recovery packet incompatible with selected candidate')
    identity = {'configuration': config, 'output_root': str(reviews_root()),
                'jobs_root': str(jobs_root()), 'consumed_root': str(output_roots()[2])}
    digest_input = identity if os.environ.get('PLANAR_EVALS_OUTPUT_ROOT') else config
    if candidate_root is not None or trial_deadline != 1500:
        digest_input = {'base': digest_input, 'candidate_root': str(candidate), 'case_root': str(case), 'trial_deadline': trial_deadline}
    if retained:
        digest_input={'base':digest_input,'source_packet_root':str(Path(source_packet).resolve(strict=True)),
                      'source_packet_sha256':source_identity['source_packet_sha256'], 'execution_id':execution_id,
                      'absolute_deadline':absolute_deadline}
    config_digest = hashlib.sha256(json.dumps(digest_input, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    task = Task(case).config
    if case_name == 'clean-revision':
        from harbor.models.task.config import NetworkMode
        from harbor.models.task.verifier_mode import resolve_step_verifier_mode, VerifierEnvironmentMode
        steps = task.steps or []
        if ([step.name for step in steps] != ['worker','revision'] or steps[0].min_reward != 1
            or any(step.agent.timeout_sec != 400 or step.verifier.timeout_sec != 60 for step in steps)
            or task.agent.timeout_sec != 400 or task.verifier.timeout_sec != 60
            or task.environment.docker_image != 'sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2'
            or task.verifier.environment is None or not re.fullmatch(r'sha256:[0-9a-f]{64}', task.verifier.environment.docker_image or '')
            or task.verifier.environment.network_mode != NetworkMode.NO_NETWORK
            or any(resolve_step_verifier_mode(task,step) != VerifierEnvironmentMode.SEPARATE for step in steps)
            or [step.verifier.env.get('PLANAR_CLEAN_PHASE') for step in steps] != ['worker','revision']
            or task.artifacts != ['/workspace/home']
            or config['agents'][0].get('resume_trajectory') is not False
            or config['agents'][0].get('load_trajectory') is not None
            or config.get('verifier',{}).get('disable') is not False):
            raise ValueError('clean revision native step contract mismatch')
        from clean_revision_support import load_case_module
        identity = load_case_module(case,'step_identity')
        frozen = identity.load_instructions(case)
        selected = config['agents'][0]
        identity.validate_selection(frozen, selected['model_name'], selected['kwargs']['reasoning_effort'])
    if retained:
        from harbor.models.task.config import NetworkMode
        from harbor.models.task.verifier_mode import VerifierEnvironmentMode
        if (task.steps or task.agent.timeout_sec != 400 or task.verifier.timeout_sec != 60
            or task.environment.docker_image != 'sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2'
            or task.verifier.environment_mode != VerifierEnvironmentMode.SEPARATE
            or task.verifier.environment is None or not re.fullmatch(r'sha256:[0-9a-f]{64}',task.verifier.environment.docker_image or '')
            or task.verifier.environment.network_mode != NetworkMode.NO_NETWORK or task.artifacts != ['/workspace/home']):
            raise ValueError('retained native task contract mismatch')
        validate_recovery_selection((case/'instruction.md').read_text(),config['agents'][0]['model_name'],config['agents'][0]['kwargs']['reasoning_effort'])
        effective=config_for(execution_id,jobs_root()/execution_id,case,effort)
        effective_hash=hashlib.sha256(json.dumps(effective,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    prerequisites = {'cwd': str(Path.cwd().resolve()), 'repository_root': str(ROOT),
        'package_read_root': str(candidate), 'case_read_root': str(case),
        'project_root': str(PROJECT), 'evidence_write_root': str(jobs_root()),
        'write_destinations': write_roots,
        'isolated_remote_roots': {'home': HOME, 'config': HOME + '/config.toml',
            'cache': CACHE, 'workspace': '/workspace',
            'status': 'configured; container setup and workspace check pending'},
        'checker_sha256': case_inventory(case / 'tests'),
        'runtime': {'python': sys.version, 'executable': sys.executable, 'platform': platform.platform(),
                    'harbor_observed': importlib.metadata.version('harbor')},
        'requested_versions': {'harbor': '0.26.0', 'codex': '0.169.0'},
        'configuration_source': 'launch.config_for and smoke_codex.CONFIG',
        'authentication_route': 'explicit subscription auth file; checked only on execute',
        'installation': 'native marketplace add, plugin add, list and installed inventory during setup; pending',
        'lifetime': 'fresh only; resume and load forbidden',
        'budgets_seconds': {'agent': task.agent.timeout_sec, 'verifier': task.verifier.timeout_sec, 'job': trial_deadline, 'grace': 15},
        'cleanup_owner': 'SmokeCodex and watchdog process group; recovery marker until confirmed',
        'last_observed_event': 'preflight plan', 'verdict': 'pending'}
    if case_name == 'clean-revision':
        prerequisites['execution_gate'] = 'requires independent review, matching expiry and one-use gate before authentication'
        prerequisites['budgets_seconds']['step_agent'] = [400,400]
        prerequisites['fixture'] = 'clean projected fixture and host-owned controlled state fault'
    if case_name == 'fresh-interrupted-review':
        prerequisites['execution_gate'] = 'requires independent review and matching one-use gate before execution'
        prerequisites['fixture'] = 'portable source-derived fixture; applicability requires independent review'
    if retained:
        prerequisites['execution_gate']='requires an independently reviewed, exact source-bound one-use gate before authentication'
        prerequisites['source_packet_root']=str(Path(source_packet).resolve(strict=True))
        prerequisites['source_packet_sha256']=source_identity['source_packet_sha256']
        prerequisites['reserved_execution_id']=execution_id
        prerequisites['absolute_deadline']=absolute_deadline
        prerequisites['account_scope_id']=str(reviews_root().resolve()/'planar-account-admission-v1')
        prerequisites['effective_configuration_sha256']=effective_hash
        prerequisites['source_online_outcome']='behavioral_failure/reward 0'
    return {'prerequisites': prerequisites, 'case': case_name, 'case_sha256': case_inventory(case),
            'image_id': Task(case).config.environment.docker_image,
            'implementation_sha256': source_digest() if case_name == 'native-lineage-portable' else source_digest((EVALS/'planar/cases/retained-a-fresh-b') if retained else CASES[case_name]), 'candidate_sha256': expected['sha256'],
            'configuration_sha256': config_digest, 'purpose': 'one-diagnostic-attempt',
            'harbor_version': '0.26.0', 'codex_version': '0.169.0', 'configuration': config}


def consume_review_once(review_path, consumed_root, *, reviewed_bytes=None):
    """Reserve the exact reviewed gate once; never retry it implicitly."""
    raw = review_path.read_bytes()
    if reviewed_bytes is not None and raw != reviewed_bytes: raise ValueError('reviewed gate changed before consumption')
    gate = json.loads(raw)
    digest = hashlib.sha256(json.dumps(gate, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    consumed_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    marker = consumed_root / digest
    try:
        fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise ValueError('diagnostic approval was already consumed') from None
    with os.fdopen(fd, 'w') as out:
        out.write('reserved\n'); out.flush(); os.fsync(out.fileno())
    dirfd=os.open(consumed_root,os.O_RDONLY|os.O_DIRECTORY)
    try: os.fsync(dirfd)
    finally: os.close(dirfd)
    return digest


def execute(auth_path, review_path, case_name='plugin-smoke', *, candidate_root=None, case_root=None, trial_deadline=1500, execution_id=None, effort='medium', absolute_deadline=None, emit=True, schema2_context=None, source_packet=None):
    kwargs={'candidate_root':candidate_root,'case_root':case_root,'trial_deadline':trial_deadline,'execution_id':execution_id,
            'effort':effort,'absolute_deadline':absolute_deadline,'emit':emit,'schema2_context':schema2_context,'source_packet':source_packet}
    if case_name != 'retained-a-fresh-b':
        return _execute_inner(auth_path,review_path,case_name,**kwargs)
    # Reuse the shared account lock and pending-record namespace, held across
    # the whole trial. Structural results stay pending until a bound raw audit.
    sys.path.insert(0,str(EVALS/'planar/experiments'))
    try:
        import codex_run
        scope=reviews_root()/'planar-account-admission-v1'
        if scope.is_symlink() or (scope/'lock').is_symlink() or (scope/'records').is_symlink():
            raise ValueError('unsafe retained account scope')
        with codex_run.admission_scope() as records:
            if codex_run.shared_pending(records): raise RuntimeError('shared account admission blocked')
            return _execute_inner(auth_path,review_path,case_name,_retained_records=records,**kwargs)
    finally:
        sys.path.remove(str(EVALS/'planar/experiments'))


def _execute_inner(auth_path, review_path, case_name='plugin-smoke', *, candidate_root=None, case_root=None, trial_deadline=1500, execution_id=None, effort='medium', absolute_deadline=None, emit=True, schema2_context=None, source_packet=None, _retained_records=None):
    retained=case_name=='retained-a-fresh-b'
    if retained and _retained_records is None: raise ValueError('retained account admission missing')
    if not retained and source_packet is not None: raise ValueError('source packet is retained-case only')
    def remaining():
        if absolute_deadline is None:
            return None
        seconds = (datetime.fromisoformat(absolute_deadline) - datetime.now(timezone.utc)).total_seconds()
        if seconds <= 0:
            raise TimeoutError('experiment absolute deadline expired')
        return seconds
    remaining()
    portable = case_name == 'native-lineage-portable'
    if portable:
        if not isinstance(schema2_context,dict) or set(schema2_context) != {'root','identity','reservation_id','preparation_evidence'} or candidate_root is None or case_root is None:
            raise ValueError('portable execution requires exact activated schema-2 context')
        sys.path.insert(0,str(EVALS/'planar/experiments'))
        import schema2_execution
        admitted=schema2_execution.verify_activated(schema2_context['root'],schema2_context['identity'],schema2_context['reservation_id'],review_path,preparation_evidence=schema2_context['preparation_evidence'])
        reservation=admitted['reservation']; binding=reservation['concrete_binding']
        if execution_id != 'planar-t044-'+reservation['execution_id'] or str(candidate_root) != binding['path_bindings']['candidate']['raw'] or str(case_root) != binding['path_bindings']['case_instance']['raw'] or effort != binding['job_config']['agents'][0]['kwargs']['reasoning_effort'] or absolute_deadline != reservation['expires_at']:
            raise ValueError('portable activated dispatch mismatch')
    elif schema2_context is not None:
        raise ValueError('schema-2 context is portable-only')
    case = Path(case_root).resolve(strict=True) if case_root is not None else CASES[case_name]
    snapshot = plan(case_name, candidate_root=candidate_root, case_root=case_root, trial_deadline=trial_deadline, effort=effort, source_packet=source_packet, execution_id=execution_id, absolute_deadline=absolute_deadline)
    if execution_id is not None and not re.fullmatch(r'planar-t044-[0-9a-f]{32}', execution_id):
        raise ValueError('invalid reserved execution ID')
    review = review_path.resolve(strict=True)
    if not review.is_relative_to(reviews_root().resolve()):
        raise ValueError('review evidence must reside under the project reviews directory')
    gate_bytes = review.read_bytes()
    gate = json.loads(gate_bytes)
    gate_hash = hashlib.sha256(gate_bytes).hexdigest()
    required = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1,
                'implementation_sha256': snapshot['implementation_sha256'],
                'candidate_sha256': snapshot['candidate_sha256'],
                'configuration_sha256': snapshot['configuration_sha256'], 'case': case_name,
                'case_sha256': snapshot['case_sha256'], 'image_id': snapshot['image_id']}
    if not portable and any(gate.get(key) != value for key, value in required.items()):
        raise ValueError('diagnostic review is missing, stale, or not approved')
    if case_name == 'clean-revision':
        if not absolute_deadline or gate.get('absolute_deadline') != absolute_deadline:
            raise ValueError('clean revision requires the matching finite gate expiry')
        try:
            expiry = datetime.fromisoformat(absolute_deadline)
            if expiry.tzinfo is None or expiry <= datetime.now(timezone.utc):
                raise ValueError()
        except (TypeError, ValueError):
            raise ValueError('clean revision gate expiry is invalid or expired') from None
    image_id = reservation['image_id'] if portable else gate.get('image_id')
    if not isinstance(image_id, str) or len(image_id) != 71 or not image_id.startswith('sha256:') or image_id != Task(case).config.environment.docker_image:
        raise ValueError('diagnostic review lacks the pinned image identity')
    verifier_image = None
    if case_name in ('clean-revision', 'native-lineage-portable', 'retained-a-fresh-b'):
        verifier_image = reservation['verifier_image_id'] if portable else gate.get('verifier_image_id')
        if (not isinstance(verifier_image, str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', verifier_image)
            or (not portable and verifier_image != Task(case).config.verifier.environment.docker_image)):
            raise ValueError('clean revision requires the reviewed verifier image identity')
    harbor = Path(sys.executable).parent / 'harbor'
    docker = shutil.which('docker')
    if not docker or not harbor.is_file():
        raise ValueError('local Docker or locked Harbor executable is unavailable')
    seconds = remaining()
    result = subprocess.run(['docker', 'image', 'inspect', '--format', '{{.Id}}', image_id], text=True, capture_output=True, timeout=min(15, seconds) if seconds is not None else 15)
    if result.returncode or result.stdout.strip() != image_id:
        raise ValueError('reviewed Docker image is unavailable')
    if verifier_image:
        result = subprocess.run(['docker', 'image', 'inspect', '--format', '{{.Id}}', verifier_image], text=True, capture_output=True, timeout=15)
        if result.returncode or result.stdout.strip() != verifier_image:
            raise ValueError('reviewed verifier image is unavailable')
    if any((jobs_root()).glob('*/recovery-required')):
        raise RuntimeError('previous diagnostic cleanup is unconfirmed')
    if portable:
        import schema2_projection
        planned_evidence=jobs_root()/execution_id
        planned_config=config_for(execution_id,planned_evidence,Path(case_root),effort)
        actual=JobConfig.model_validate(planned_config)
        if not schema2_projection.exact(schema2_projection.project(actual),binding['job_config']):
            raise ValueError('portable concrete JobConfig mismatch before auth')
        caps=binding['caps']
        budgets=snapshot['prerequisites']['budgets_seconds']
        if trial_deadline > caps['trial_seconds'] or budgets['agent'] > caps['session_seconds'] or budgets['verifier'] > caps['verifier_seconds']:
            raise ValueError('portable runtime exceeds reviewed caps')
    if retained:
        from retained_b_support import validate_gate, source as validate_retained_source
        policy=json.loads((case/'source-policy.json').read_text())
        validate_gate(gate,snapshot,policy,execution_id,absolute_deadline,auth_path,_retained_records.parent,verifier_image)
        planned_evidence=jobs_root()/execution_id
        if planned_evidence.exists() or planned_evidence.is_symlink(): raise ValueError('retained execution already exists')
        planned_config=config_for(execution_id,planned_evidence,case,effort)
        actual_hash=hashlib.sha256(json.dumps(planned_config,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        if actual_hash!=snapshot['prerequisites']['effective_configuration_sha256']:
            raise ValueError('retained effective JobConfig drift')
        if validate_retained_source(case,source_packet,Path(candidate_root))['source_packet_sha256']!=snapshot['prerequisites']['source_packet_sha256']:
            raise ValueError('retained source changed before admission')
        consume_review_once(review, reviews_root()/'harbor-t044-consumed', reviewed_bytes=gate_bytes)
        marker=_retained_records/(execution_id+'.start.json')
        fd=os.open(marker,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'w') as output:
            json.dump({'execution_id':execution_id,'case':case_name,'gate_sha256':gate_hash,'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'},output,sort_keys=True)
            output.write('\n'); output.flush(); os.fsync(output.fileno())
        dirfd=os.open(_retained_records,os.O_RDONLY|os.O_DIRECTORY)
        try: os.fsync(dirfd)
        finally: os.close(dirfd)
    if case_name == 'clean-revision':
        # The selected multi-step case consumes its reviewed one-use gate before
        # resolving or reading the authentication file.
        consume_review_once(review, reviews_root() / 'harbor-t044-consumed')
    if portable:
        schema2_execution.verify_activated(schema2_context['root'],schema2_context['identity'],schema2_context['reservation_id'],review,preparation_evidence=schema2_context['preparation_evidence'])
    if retained and review.read_bytes() != gate_bytes: raise ValueError('reviewed gate changed before auth')
    if retained and (Path(auth_path).is_symlink() or Path(auth_path).resolve(strict=True) != Path(auth_path)):
        raise ValueError('retained auth path must be canonical and non-symlinked')
    auth_path = auth_path.resolve(strict=True)
    # Read solely to enforce the subscription route; never retain or print bytes.
    try:
        auth = json.loads(auth_path.read_text())
        if not isinstance(auth, dict) or auth.get('auth_mode') != 'chatgpt' or not isinstance(auth.get('tokens'), dict) or not auth['tokens'].get('access_token'):
            raise ValueError('subscription file authentication is unavailable')
    except (OSError, UnicodeError, json.JSONDecodeError):
        raise ValueError('subscription file authentication is unavailable') from None
    del auth
    remaining()
    if case_name not in ('clean-revision', 'native-lineage-portable', 'retained-a-fresh-b'):
        consume_review_once(review, reviews_root() / 'harbor-t044-consumed')
    run_id = execution_id or 'planar-t044-' + uuid.uuid4().hex
    evidence = jobs_root() / run_id
    evidence.mkdir(parents=True, exist_ok=False, mode=0o700)
    job_config = planned_config if (portable or retained) else config_for(run_id, evidence, case, effort)
    if portable:
        # Revalidate the gate and compare the actual concrete object after all
        # launcher mutations before passing it to native_job.
        schema2_execution.verify_activated(schema2_context['root'],schema2_context['identity'],schema2_context['reservation_id'],review,preparation_evidence=schema2_context['preparation_evidence'])
        import schema2_projection
        actual = JobConfig.model_validate(job_config)
        if not schema2_projection.exact(schema2_projection.project(actual),binding['job_config']):
            raise ValueError('portable concrete JobConfig mismatch')
    config_path = evidence / 'job-config.json'
    if portable:
        config_path.write_bytes(schema2_projection.canonical(binding['job_config']))
    else:
        config_path.write_text(json.dumps(job_config, indent=2, sort_keys=True) + '\n')
    launcher_record = {'execution_id': run_id, 'started_at': datetime.now(timezone.utc).isoformat(), 'case': case_name, 'case_sha256': snapshot['case_sha256'], 'image_id': image_id, 'configuration_sha256': snapshot['configuration_sha256'], 'implementation_sha256': snapshot['implementation_sha256'], 'candidate_sha256': snapshot['candidate_sha256'], 'harbor_version': '0.26.0', 'codex_version': '0.169.0', 'prerequisites': snapshot['prerequisites'], 'review_sha256': gate_hash, 'job_config_sha256': hashlib.sha256(config_path.read_bytes()).hexdigest()}
    if portable:
        launcher_record.update({'runnable_contract':'planar-schema2-runnable-v1','verifier_image_id':verifier_image,'effective_verifier_environment':binding['effective_verifier_environment']})
    (evidence / 'launcher.json').write_text(json.dumps(launcher_record, indent=2, sort_keys=True) + '\n')
    if retained:
        admission={'execution_id':run_id,'gate_path':str(review),'gate_raw_sha256':gate_hash,'gate_authority':gate,
            'source_packet_sha256':snapshot['prerequisites']['source_packet_sha256'],'source_packet_root':snapshot['prerequisites']['source_packet_root'],
            'account_scope_id':str(_retained_records.parent),'case_sha256':snapshot['case_sha256'],
            'candidate_sha256':snapshot['candidate_sha256'],'effective_configuration_sha256':snapshot['prerequisites']['effective_configuration_sha256'],
            'verifier_image_id':verifier_image}
        fd=os.open(evidence/'retained-admission.json',os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'w') as output: json.dump(admission,output,sort_keys=True); output.write('\n'); output.flush(); os.fsync(output.fileno())
    # Docker uses the current user's context; no API-key or host configuration
    # variables are inherited by the Harbor process.
    directories = [str(Path(docker).resolve().parent), str(Path(sys.executable).parent), '/usr/bin', '/bin', '/usr/sbin', '/sbin']
    env = {'PATH': os.pathsep.join(directories), 'HOME': str(Path.home()),
           'PYTHONPATH': str(Path(__file__).resolve().parent),
           'PLANAR_T044_EVIDENCE_DIR': str(evidence),
           'PLANAR_EVALS_OUTPUT_ROOT': os.environ.get('PLANAR_EVALS_OUTPUT_ROOT', ''),
           'PLANAR_T044_EXPECTED_IMAGE_ID': image_id, 'PLANAR_T044_CASE': case_name,
           'PLANAR_T044_DEADLINE_SECONDS': str(trial_deadline), 'PLANAR_T044_EFFORT': effort,
           'CODEX_AUTH_JSON_PATH': str(auth_path), 'OPENAI_API_KEY': '', 'CODEX_API_KEY': ''}
    if verifier_image:
        env['PLANAR_T045_VERIFIER_IMAGE_ID'] = verifier_image
    if retained:
        env['PLANAR_T045_SOURCE_PACKET']=str(Path(source_packet).resolve(strict=True))
        env['PLANAR_T045_SOURCE_SHA256']=snapshot['prerequisites']['source_packet_sha256']
    if portable:
        context_path=evidence/'schema2-context.json'
        context_path.write_text(json.dumps({**schema2_context,'gate_path':str(review)},sort_keys=True)+'\n')
        env['PLANAR_SCHEMA2_CONTEXT'] = str(context_path)
        env['PYTHONPATH'] += os.pathsep + str(EVALS/'planar/experiments')
    if candidate_root is not None:
        env['PLANAR_T044_CANDIDATE_SNAPSHOT'] = str(Path(candidate_root).resolve(strict=True))
        env['PLANAR_T044_CASE_SNAPSHOT'] = str(case)
        env['PLANAR_T044_CASE_SHA256'] = snapshot['case_sha256']
    command = [sys.executable, str(Path(__file__).with_name('native_job.py')), str(config_path), str(evidence)]
    (evidence / 'launcher-command.json').write_text(json.dumps({'execution_id': run_id, 'argv': command, 'environment_keys': sorted(env)}, indent=2) + '\n')
    marker = evidence / 'recovery-required'
    marker.write_text('cleanup unconfirmed\n')
    (evidence / 'projects').mkdir(mode=0o700)
    env['PLANAR_T044_PARENT_PID'] = str(os.getpid())
    if absolute_deadline is not None:
        env['PLANAR_T044_ABSOLUTE_DEADLINE'] = absolute_deadline
    watchdog = [sys.executable, str(Path(__file__).with_name('watchdog.py'))]
    remaining()
    child = subprocess.Popen(watchdog, cwd=ROOT, env=env, stdout=sys.stderr if not emit else None)
    handlers = {}
    def forward(signum, frame):
        child.send_signal(signum)
    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            handlers[signum] = signal.signal(signum, forward)
        child.wait()
        watchdog_file = evidence / 'watchdog-terminal.json'
        status = json.loads(watchdog_file.read_text()) if watchdog_file.is_file() else {'status': 'watchdog_missing'}
    except BaseException:
        (evidence / 'launcher-terminal.json').write_text(json.dumps({'execution_id': run_id, 'status': 'interrupted_cleanup_unproven'}, indent=2) + '\n')
        raise
    finally:
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
    (evidence / 'launcher-terminal.json').write_text(json.dumps({'execution_id': run_id, 'finished_at': datetime.now(timezone.utc).isoformat(), **status}, indent=2) + '\n')
    result = {'execution_id': run_id, 'evidence': str(evidence), 'diagnostic_verdict': 'pending', **status}
    if emit:
        print(json.dumps(result))
        return 2
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--auth-file', type=Path)
    parser.add_argument('--review-file', type=Path)
    parser.add_argument('--case', choices=tuple(CASES), default='plugin-smoke')
    parser.add_argument('--snapshot-candidate', type=Path)
    parser.add_argument('--snapshot-case', type=Path)
    parser.add_argument('--source-packet', type=Path)
    parser.add_argument('--trial-deadline', type=int, default=1500)
    parser.add_argument('--execution-id')
    parser.add_argument('--absolute-deadline')
    parser.add_argument('--effort', choices=('medium', 'high'), default='medium')
    args = parser.parse_args()
    if not args.execute:
        print(json.dumps(plan(args.case, candidate_root=args.snapshot_candidate, case_root=args.snapshot_case, trial_deadline=args.trial_deadline, effort=args.effort, source_packet=args.source_packet, execution_id=args.execution_id, absolute_deadline=args.absolute_deadline), indent=2, sort_keys=True))
        return 0
    if args.auth_file is None or args.review_file is None:
        parser.error('--execute requires --auth-file and --review-file')
    return execute(args.auth_file, args.review_file, args.case, candidate_root=args.snapshot_candidate, case_root=args.snapshot_case, trial_deadline=args.trial_deadline, execution_id=args.execution_id, effort=args.effort, absolute_deadline=args.absolute_deadline, source_packet=args.source_packet)


if __name__ == '__main__':
    sys.exit(main())
