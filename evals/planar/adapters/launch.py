"""Bounded Harbor plan and review-gated diagnostic launcher for T-044."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
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
CASES = {name: EVALS / 'planar/cases' / name for name in ('plugin-smoke', 'native-lineage-smoke', 'fresh-interrupted-review')}


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
    records = []
    for path in sorted(paths):
        if path.is_symlink():
            raise ValueError('symlink in evaluation source')
        records.append((path.relative_to(ROOT).as_posix(), hashlib.sha256(path.read_bytes()).hexdigest()))
    return hashlib.sha256(json.dumps(records, separators=(',', ':')).encode()).hexdigest()


def config_for(job_name, jobs_dir, case=CASE):
    data = {'job_name': job_name, 'jobs_dir': str(jobs_dir), 'n_attempts': 1,
            'n_concurrent_trials': 1, 'retry': {'max_retries': 0},
            'environment': {'import_path': 'guarded_docker:GuardedDocker', 'delete': True, 'kwargs': {'auto_mounting': False}},
            'agents': [{'import_path': 'smoke_codex:SmokeCodex', 'model_name': 'gpt-6.1-terra',
                        'kwargs': {'version': '0.169.0', 'config': CONFIG, 'reasoning_effort': 'medium'}}],
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


def plan(case_name='plugin-smoke'):
    case = CASES[case_name]
    if Path.cwd().resolve() != ROOT.resolve():
        raise ValueError('launch must run from the repository root')
    for label, path in (('repository', ROOT), ('package', CANDIDATE), ('case', case), ('project', ROOT if os.environ.get('PLANAR_EVALS_OUTPUT_ROOT') else PROJECT),):
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
    if not (git_dir / 'HEAD').is_file() or not (CANDIDATE / '.claude-plugin/plugin.json').is_file():
        raise ValueError('repository or package root is invalid')
    write_roots = {'harbor-t044-jobs': validate_write_destination(jobs_root()),
                   'harbor-t044-consumed': validate_write_destination(reviews_root() / 'harbor-t044-consumed')}
    expected = json.loads((case / 'candidate.json').read_text())
    if inventory(CANDIDATE) != expected:
        raise ValueError('candidate differs from recorded frozen inventory')
    config = config_for('planar-t044-plan', jobs_root(), case)
    if case_name == 'fresh-interrupted-review':
        selected = config['agents'][0]
        validate_recovery_selection((case / 'instruction.md').read_text(), selected['model_name'], selected['kwargs']['reasoning_effort'])
        compatibility = runpy.run_path(str(case / 'compatibility.py'))['inspect'](CANDIDATE, case / 'fixture-v2')
        if compatibility['missing_packet_sections'] or compatibility['immutable_replacement_required']:
            raise ValueError('v2 recovery packet incompatible with selected candidate')
    identity = {'configuration': config, 'output_root': str(reviews_root()),
                'jobs_root': str(jobs_root()), 'consumed_root': str(output_roots()[2])}
    digest_input = identity if os.environ.get('PLANAR_EVALS_OUTPUT_ROOT') else config
    config_digest = hashlib.sha256(json.dumps(digest_input, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    task = Task(case).config
    prerequisites = {'cwd': str(Path.cwd().resolve()), 'repository_root': str(ROOT),
        'package_read_root': str(CANDIDATE), 'case_read_root': str(case),
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
        'budgets_seconds': {'agent': task.agent.timeout_sec, 'verifier': task.verifier.timeout_sec, 'job': 1500, 'grace': 15},
        'cleanup_owner': 'SmokeCodex and watchdog process group; recovery marker until confirmed',
        'last_observed_event': 'preflight plan', 'verdict': 'pending'}
    if case_name == 'fresh-interrupted-review':
        prerequisites['execution_gate'] = 'requires independent review and matching one-use gate before execution'
        prerequisites['fixture'] = 'portable source-derived fixture; applicability requires independent review'
    return {'prerequisites': prerequisites, 'case': case_name, 'case_sha256': case_inventory(case),
            'image_id': Task(case).config.environment.docker_image,
            'implementation_sha256': source_digest(case), 'candidate_sha256': expected['sha256'],
            'configuration_sha256': config_digest, 'purpose': 'one-diagnostic-attempt',
            'harbor_version': '0.26.0', 'codex_version': '0.169.0', 'configuration': config}


def consume_review_once(review_path, consumed_root):
    """Reserve the exact reviewed gate once; never retry it implicitly."""
    gate = json.loads(review_path.read_text())
    digest = hashlib.sha256(json.dumps(gate, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    consumed_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    marker = consumed_root / digest
    try:
        fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise ValueError('diagnostic approval was already consumed') from None
    with os.fdopen(fd, 'w') as out:
        out.write('reserved\n')
    return digest


def execute(auth_path, review_path, case_name='plugin-smoke'):
    case = CASES[case_name]
    snapshot = plan(case_name)
    review = review_path.resolve(strict=True)
    if not review.is_relative_to(reviews_root().resolve()):
        raise ValueError('review evidence must reside under the project reviews directory')
    gate = json.loads(review.read_text())
    required = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1,
                'implementation_sha256': snapshot['implementation_sha256'],
                'candidate_sha256': snapshot['candidate_sha256'],
                'configuration_sha256': snapshot['configuration_sha256'], 'case': case_name,
                'case_sha256': snapshot['case_sha256'], 'image_id': snapshot['image_id']}
    if any(gate.get(key) != value for key, value in required.items()):
        raise ValueError('diagnostic review is missing, stale, or not approved')
    image_id = gate.get('image_id')
    if not isinstance(image_id, str) or len(image_id) != 71 or not image_id.startswith('sha256:') or image_id != Task(case).config.environment.docker_image:
        raise ValueError('diagnostic review lacks the pinned image identity')
    harbor = Path(sys.executable).parent / 'harbor'
    docker = shutil.which('docker')
    if not docker or not harbor.is_file():
        raise ValueError('local Docker or locked Harbor executable is unavailable')
    result = subprocess.run(['docker', 'image', 'inspect', '--format', '{{.Id}}', image_id], text=True, capture_output=True, timeout=15)
    if result.returncode or result.stdout.strip() != image_id:
        raise ValueError('reviewed Docker image is unavailable')
    if any((jobs_root()).glob('*/recovery-required')):
        raise RuntimeError('previous diagnostic cleanup is unconfirmed')
    auth_path = auth_path.resolve(strict=True)
    # Read solely to enforce the subscription route; never retain or print bytes.
    try:
        auth = json.loads(auth_path.read_text())
        if not isinstance(auth, dict) or auth.get('auth_mode') != 'chatgpt' or not isinstance(auth.get('tokens'), dict) or not auth['tokens'].get('access_token'):
            raise ValueError('subscription file authentication is unavailable')
    except (OSError, UnicodeError, json.JSONDecodeError):
        raise ValueError('subscription file authentication is unavailable') from None
    del auth
    consume_review_once(review, reviews_root() / 'harbor-t044-consumed')
    run_id = 'planar-t044-' + uuid.uuid4().hex
    evidence = jobs_root() / run_id
    evidence.mkdir(parents=True, exist_ok=False, mode=0o700)
    job_config = config_for(run_id, evidence, case)
    config_path = evidence / 'job-config.json'
    config_path.write_text(json.dumps(job_config, indent=2, sort_keys=True) + '\n')
    (evidence / 'launcher.json').write_text(json.dumps({'execution_id': run_id, 'started_at': datetime.now(timezone.utc).isoformat(), 'case': case_name, 'case_sha256': snapshot['case_sha256'], 'image_id': image_id, 'configuration_sha256': snapshot['configuration_sha256'], 'implementation_sha256': snapshot['implementation_sha256'], 'candidate_sha256': snapshot['candidate_sha256'], 'harbor_version': '0.26.0', 'codex_version': '0.169.0', 'prerequisites': snapshot['prerequisites'], 'review_sha256': hashlib.sha256(review.read_bytes()).hexdigest(), 'job_config_sha256': hashlib.sha256(config_path.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + '\n')
    # Docker uses the current user's context; no API-key or host configuration
    # variables are inherited by the Harbor process.
    directories = [str(Path(docker).resolve().parent), str(Path(sys.executable).parent), '/usr/bin', '/bin', '/usr/sbin', '/sbin']
    env = {'PATH': os.pathsep.join(directories), 'HOME': str(Path.home()),
           'PYTHONPATH': str(Path(__file__).resolve().parent),
           'PLANAR_T044_EVIDENCE_DIR': str(evidence),
           'PLANAR_EVALS_OUTPUT_ROOT': os.environ.get('PLANAR_EVALS_OUTPUT_ROOT', ''),
           'PLANAR_T044_EXPECTED_IMAGE_ID': image_id, 'PLANAR_T044_CASE': case_name,
           'CODEX_AUTH_JSON_PATH': str(auth_path), 'OPENAI_API_KEY': '', 'CODEX_API_KEY': ''}
    command = [str(harbor), 'run', '--config', str(config_path)]
    (evidence / 'launcher-command.json').write_text(json.dumps({'execution_id': run_id, 'argv': command, 'environment_keys': sorted(env)}, indent=2) + '\n')
    marker = evidence / 'recovery-required'
    marker.write_text('cleanup unconfirmed\n')
    (evidence / 'projects').mkdir(mode=0o700)
    env['PLANAR_T044_PARENT_PID'] = str(os.getpid())
    watchdog = [sys.executable, str(Path(__file__).with_name('watchdog.py'))]
    child = subprocess.Popen(watchdog, cwd=ROOT, env=env)
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
    print(json.dumps({'execution_id': run_id, 'evidence': str(evidence), 'diagnostic_verdict': 'pending', **status}))
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--auth-file', type=Path)
    parser.add_argument('--review-file', type=Path)
    parser.add_argument('--case', choices=tuple(CASES), default='plugin-smoke')
    args = parser.parse_args()
    if not args.execute:
        print(json.dumps(plan(args.case), indent=2, sort_keys=True))
        return 0
    if args.auth_file is None or args.review_file is None:
        parser.error('--execute requires --auth-file and --review-file')
    return execute(args.auth_file, args.review_file, args.case)


if __name__ == '__main__':
    sys.exit(main())
