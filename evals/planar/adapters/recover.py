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
    evidence = validate_owned_evidence(evidence)
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
    """Resolve the explicitly selected project and output context."""
    import os
    from pathlib import Path
    from eval_context import resolve
    repo = Path(__file__).resolve().parents[3]
    context = resolve(repo, historical=os.environ.get('PLANAR_EVALS_LAYOUT_KIND') in ('legacy-reviews', 'override-jobs'))
    return Path(context.output_root), Path(context.jobs_root), Path(context.consumed_root)


def jobs_root():
    """Evidence jobs root; override must also be supplied for manual recovery."""
    return output_roots()[1]


def validate_owned_evidence(evidence):
    """Verify context and exact recorded evidence before any cleanup or write."""
    import os
    from pathlib import Path
    value = Path(evidence)
    if not value.is_absolute() or value.is_symlink() or value != value.resolve(strict=True):
        raise ValueError('unsafe evidence path')
    evidence = value
    root = jobs_root()
    if evidence.parent != root or not (evidence / 'recovery-required').is_file() or (evidence / 'recovery-required').is_symlink():
        raise ValueError('no registered project recovery marker')
    # A pathname alone is not proof of the historical identity. Require the
    # immutable launcher and its bound config to name this exact job root.
    import hashlib
    launcher_file, config_file = evidence / 'launcher.json', evidence / 'job-config.json'
    if any(p.is_symlink() or not p.is_file() for p in (launcher_file, config_file)):
        raise ValueError('launcher and job config are required')
    launcher = json.loads(launcher_file.read_text())
    raw = config_file.read_bytes()
    config = json.loads(raw)
    if (launcher.get('execution_id') != evidence.name or
        launcher.get('job_config_sha256') != hashlib.sha256(raw).hexdigest() or
        Path(config.get('jobs_dir', '')).resolve() != evidence):
        raise ValueError('recorded job root or identity mismatch')
    from eval_context import resolve
    historical = os.environ.get('PLANAR_EVALS_LAYOUT_KIND') in ('legacy-reviews','override-jobs')
    selection = resolve(Path(__file__).resolve().parents[3], historical=historical)
    prerequisites = launcher.get('prerequisites')
    if (not isinstance(prerequisites, dict) or prerequisites.get('project_root') != selection.project_dir or
        prerequisites.get('evidence_write_root') != selection.jobs_root):
        raise ValueError('project and jobs roots not bound in launcher record')
    if not historical:
        if not os.environ.get('PLANAR_EVALS_CONTEXT'):
            raise ValueError('forwarded context is required for new watchdog cleanup')
        if (prerequisites.get('selection_context') != selection.identity() or
            prerequisites.get('selection_context_sha256') != selection.digest()):
            raise ValueError('recorded context mismatch')
    else:
        review_value = os.environ.get('PLANAR_EVALS_REVIEW_PATH')
        if not review_value: raise ValueError('historical recovery requires the exact recorded review path')
        review = Path(review_value)
        if (not review.is_absolute() or review.is_symlink() or not review.is_file() or
            review != review.resolve() or not review.is_relative_to(Path(selection.output_root))):
            raise ValueError('unsafe historical review path')
        review_raw = review.read_bytes()
        if hashlib.sha256(review_raw).hexdigest() != launcher.get('review_sha256'):
            raise ValueError('historical review hash mismatch')
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result: raise ValueError('duplicate historical gate field')
                result[key] = value
            return result
        gate = json.loads(review_raw, object_pairs_hook=unique)
        marker_id = hashlib.sha256(json.dumps(gate, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        marker = Path(selection.consumed_root)/marker_id
        if marker.is_symlink() or not marker.is_file() or marker.read_bytes() != b'reserved\n':
            raise ValueError('historical consumed gate marker missing')
    return evidence


def manual_recover(evidence):
    """Recover an exact historical job after the Harbor process is absent."""
    import os
    if os.environ.get('PLANAR_EVALS_LAYOUT_KIND') not in ('legacy-reviews', 'override-jobs'):
        raise ValueError('manual recovery requires exact historical root and layout')
    evidence = validate_owned_evidence(evidence)
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
