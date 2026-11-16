"""Recover exactly recorded disposable containers; no broad Docker sweeps.

Prototype: the diagnostic launcher does not invoke this yet. A future launcher
must durably write the full identity record before any credential transfer.
"""
from __future__ import annotations

import json
import re
import subprocess

CONTAINER_ID = re.compile(r'^[0-9a-f]{64}$')
PROJECT = re.compile(r'^[a-z0-9][a-z0-9_-]+$')


def recover(record, run=subprocess.run):
    containers = record.get('containers')
    if not isinstance(containers, list) or not containers:
        raise ValueError('exact recorded container identities are required')
    for item in containers:
        cid, project, image = item.get('id'), item.get('compose_project'), item.get('image_id')
        if not isinstance(cid, str) or not CONTAINER_ID.fullmatch(cid) or not isinstance(project, str) or not PROJECT.fullmatch(project) or not isinstance(image, str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', image):
            raise ValueError('invalid recorded container identity')
        query = run(['docker', 'inspect', '--format', '{{json .}}', cid], capture_output=True, text=True, timeout=15)
        if query.returncode:
            # Absence must be confirmed, not guessed from an arbitrary daemon error.
            verify = run(['docker', 'container', 'ls', '-aq', '--no-trunc', '--filter', 'id=' + cid], capture_output=True, text=True, timeout=15)
            if verify.returncode or verify.stdout.strip():
                raise RuntimeError('container absence not confirmed')
            continue
        current = json.loads(query.stdout)
        if current.get('Id') != cid or current.get('Image') != image or current.get('Config', {}).get('Labels', {}).get('com.docker.compose.project') != project:
            raise RuntimeError('container identity mismatch; refusing removal')
        removed = run(['docker', 'rm', '-f', cid], capture_output=True, text=True, timeout=30)
        if removed.returncode:
            raise RuntimeError('container removal failed')
        verify = run(['docker', 'container', 'ls', '-aq', '--no-trunc', '--filter', 'id=' + cid], capture_output=True, text=True, timeout=15)
        if verify.returncode or verify.stdout.strip():
            raise RuntimeError('container removal not confirmed')


def recover_projects(evidence, run=subprocess.run):
    """Recover projects registered by GuardedDocker before container creation."""
    from pathlib import Path
    evidence = Path(evidence).resolve(strict=True)
    registry = evidence / 'projects'
    if not registry.is_dir() or registry.is_symlink():
        raise RuntimeError('project registry unavailable; manual audit required')
    prefix = evidence.name.lower() + '-'
    for path in registry.iterdir():
        if path.is_symlink() or not path.is_file() or path.suffix != '.json':
            raise RuntimeError('invalid project registry entry')
        data = json.loads(path.read_text())
        project = data.get('compose_project')
        if data.get('execution_id') != evidence.name or not isinstance(project, str) or not project.startswith(prefix) or not PROJECT.fullmatch(project):
            raise RuntimeError('project identity mismatch')
        result = run(['docker', 'container', 'ls', '-aq', '--no-trunc', '--filter', 'label=com.docker.compose.project=' + project], capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError('could not enumerate project containers')
        for cid in result.stdout.splitlines():
            if not CONTAINER_ID.fullmatch(cid):
                raise RuntimeError('invalid discovered container identity')
            inspected = run(['docker', 'inspect', '--format', '{{json .}}', cid], capture_output=True, text=True, timeout=15)
            if inspected.returncode:
                raise RuntimeError('container identity inspection failed')
            details = json.loads(inspected.stdout)
            image = details.get('Image')
            recover({'containers': [{'id': cid, 'image_id': image, 'compose_project': project}]}, run)
        verify = run(['docker', 'container', 'ls', '-aq', '--no-trunc', '--filter', 'label=com.docker.compose.project=' + project], capture_output=True, text=True, timeout=15)
        if verify.returncode or verify.stdout.strip():
            raise RuntimeError('project cleanup not confirmed')
        # Compose networks and named volumes are also run-scoped resources.
        for kind in ('network', 'volume'):
            listing = run(['docker', kind, 'ls', '-q', '--filter', 'label=com.docker.compose.project=' + project], capture_output=True, text=True, timeout=15)
            if listing.returncode:
                raise RuntimeError('could not enumerate project ' + kind + 's')
            for identity in listing.stdout.splitlines():
                if not re.fullmatch(r'[A-Za-z0-9_.-]+', identity):
                    raise RuntimeError('invalid resource identity')
                inspected = run(['docker', kind, 'inspect', '--format', '{{json .}}', identity], capture_output=True, text=True, timeout=15)
                if inspected.returncode:
                    raise RuntimeError('resource inspection failed')
                resource = json.loads(inspected.stdout)
                if resource.get('Labels', {}).get('com.docker.compose.project') != project:
                    raise RuntimeError('resource label mismatch; refusing removal')
                removed = run(['docker', kind, 'rm', identity], capture_output=True, text=True, timeout=30)
                if removed.returncode:
                    raise RuntimeError('project resource removal failed')
            again = run(['docker', kind, 'ls', '-q', '--filter', 'label=com.docker.compose.project=' + project], capture_output=True, text=True, timeout=15)
            if again.returncode or again.stdout.strip():
                raise RuntimeError('project resource cleanup not confirmed')


def output_roots():
    """Resolve the selected root, refusing aliases for its owned directories."""
    import os
    from pathlib import Path
    override = os.environ.get('PLANAR_EVALS_OUTPUT_ROOT')
    if not override:
        reviews = Path(__file__).resolve().parents[3].parent / 'reviews'
        return reviews, reviews / 'harbor-t044-jobs', reviews / 'harbor-t044-consumed'
    root = Path(override).expanduser().resolve()
    jobs, consumed = root / 'jobs', root / 'harbor-t044-consumed'
    for path in (jobs, consumed):
        if path.is_symlink() or path.resolve() != path:
            raise ValueError(f'output destination is a symlink alias: {path}')
    return root, jobs, consumed


def jobs_root():
    """Evidence jobs root; override must also be supplied for manual recovery."""
    return output_roots()[1]


def manual_recover(evidence):
    """Recover after watchdog death only after the Harbor process is absent."""
    import os
    from pathlib import Path
    evidence = Path(evidence).resolve(strict=True)
    root = jobs_root()
    if not evidence.is_relative_to(root.resolve()) or not (evidence / 'recovery-required').is_file():
        raise ValueError('no registered project recovery marker')
    process_file = evidence / 'harbor-process.json'
    if process_file.is_symlink() or not process_file.is_file():
        raise RuntimeError('Harbor process identity missing; manual audit required')
    data = json.loads(process_file.read_text())
    pid = data.get('pid')
    if data.get('execution_id') != evidence.name or not isinstance(pid, int) or pid <= 1:
        raise RuntimeError('Harbor process identity invalid')
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        pass
    except PermissionError:
        raise RuntimeError('Harbor process state unavailable') from None
    else:
        raise RuntimeError('recorded process is still present; stop it and audit before recovery')
    recover_projects(evidence)
    (evidence / 'recovery-required').unlink()


if __name__ == '__main__':
    import sys
    manual_recover(sys.argv[1])
    print(json.dumps({'status': 'cleanup_confirmed'}))
