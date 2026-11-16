"""Bounded Harbor plan and review-gated diagnostic launcher for T-044."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import uuid

from harbor.models.job.config import JobConfig
from harbor.models.task.task import Task
from candidate import inventory
from smoke_codex import CANDIDATE, CONFIG, MANIFEST, ROOT
from supervise import supervise
from recover import recover_projects

PROJECT = ROOT.parent
REVIEWS = PROJECT / 'reviews'
EVALS = ROOT / 'evals'
CASE = EVALS / 'planar/cases/plugin-smoke'


def source_digest():
    paths = [EVALS / 'pyproject.toml', EVALS / 'uv.lock', EVALS / 'README.md', EVALS / 'planar/README.md']
    for directory in (EVALS / 'planar/adapters', CASE):
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


def config_for(job_name, jobs_dir):
    data = {'job_name': job_name, 'jobs_dir': str(jobs_dir), 'n_attempts': 1,
            'n_concurrent_trials': 1, 'retry': {'max_retries': 0},
            'environment': {'import_path': 'guarded_docker:GuardedDocker', 'delete': True, 'kwargs': {'auto_mounting': False}},
            'agents': [{'import_path': 'smoke_codex:SmokeCodex', 'model_name': 'gpt-6.1-terra',
                        'kwargs': {'version': '0.169.0', 'config': CONFIG, 'reasoning_effort': 'medium'}}],
            'tasks': [{'path': str(CASE)}]}
    return JobConfig.model_validate(data).model_dump(mode='json', exclude_none=True)


def plan():
    expected = json.loads(MANIFEST.read_text())
    if inventory(CANDIDATE) != expected:
        raise ValueError('candidate differs from recorded frozen inventory')
    config = config_for('planar-t044-plan', REVIEWS / 'harbor-t044-jobs')
    config_digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return {'implementation_sha256': source_digest(), 'candidate_sha256': expected['sha256'],
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


def execute(auth_path, review_path):
    snapshot = plan()
    review = review_path.resolve(strict=True)
    if not review.is_relative_to(REVIEWS.resolve()):
        raise ValueError('review evidence must reside under the project reviews directory')
    gate = json.loads(review.read_text())
    required = {'verdict': 'approve', 'purpose': 'one-diagnostic-attempt', 'max_attempts': 1,
                'implementation_sha256': snapshot['implementation_sha256'],
                'candidate_sha256': snapshot['candidate_sha256'],
                'configuration_sha256': snapshot['configuration_sha256']}
    if any(gate.get(key) != value for key, value in required.items()):
        raise ValueError('diagnostic review is missing, stale, or not approved')
    image_id = gate.get('image_id')
    if not isinstance(image_id, str) or len(image_id) != 71 or not image_id.startswith('sha256:') or image_id != Task(CASE).config.environment.docker_image:
        raise ValueError('diagnostic review lacks the pinned image identity')
    result = subprocess.run(['docker', 'image', 'inspect', '--format', '{{.Id}}', image_id], text=True, capture_output=True, timeout=15)
    if result.returncode or result.stdout.strip() != image_id:
        raise ValueError('reviewed Docker image is unavailable')
    if any((REVIEWS / 'harbor-t044-jobs').glob('*/recovery-required')):
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
    consume_review_once(review, REVIEWS / 'harbor-t044-consumed')
    run_id = 'planar-t044-' + uuid.uuid4().hex
    evidence = REVIEWS / 'harbor-t044-jobs' / run_id
    evidence.mkdir(parents=True, exist_ok=False, mode=0o700)
    job_config = config_for(run_id, evidence)
    config_path = evidence / 'job-config.json'
    config_path.write_text(json.dumps(job_config, indent=2, sort_keys=True) + '\n')
    (evidence / 'launcher.json').write_text(json.dumps({'execution_id': run_id, 'implementation_sha256': snapshot['implementation_sha256'], 'candidate_sha256': snapshot['candidate_sha256'], 'harbor_version': '0.26.0', 'codex_version': '0.169.0', 'review_sha256': hashlib.sha256(review.read_bytes()).hexdigest(), 'job_config_sha256': hashlib.sha256(config_path.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + '\n')
    harbor = Path(sys.executable).parent / 'harbor'
    docker = shutil.which('docker')
    if not docker or not harbor.is_file():
        raise ValueError('local Docker or locked Harbor executable is unavailable')
    # Docker uses the current user's context; no API-key or host configuration
    # variables are inherited by the Harbor process.
    directories = [str(Path(docker).resolve().parent), str(Path(sys.executable).parent), '/usr/bin', '/bin', '/usr/sbin', '/sbin']
    env = {'PATH': os.pathsep.join(directories), 'HOME': str(Path.home()),
           'PYTHONPATH': str(Path(__file__).resolve().parent),
           'PLANAR_T044_EVIDENCE_DIR': str(evidence),
           'PLANAR_T044_EXPECTED_IMAGE_ID': image_id,
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
    (evidence / 'launcher-terminal.json').write_text(json.dumps({'execution_id': run_id, **status}, indent=2) + '\n')
    print(json.dumps({'execution_id': run_id, 'evidence': str(evidence), 'diagnostic_verdict': 'pending', **status}))
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--auth-file', type=Path)
    parser.add_argument('--review-file', type=Path)
    args = parser.parse_args()
    if not args.execute:
        print(json.dumps(plan(), indent=2, sort_keys=True))
        return 0
    if args.auth_file is None or args.review_file is None:
        parser.error('--execute requires --auth-file and --review-file')
    return execute(args.auth_file, args.review_file)


if __name__ == '__main__':
    sys.exit(main())
