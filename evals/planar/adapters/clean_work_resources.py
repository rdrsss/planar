"""Narrow clean-work native project coverage; account admission remains separate."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from terminal_failure_schema import unique


def check_projects(evidence, trial_name, trial, *, run=subprocess.run):
    """Check all possible labels, requiring registrations for observed starts.

    A missing or malformed native step list is deliberately unsupported. Docker
    queries cover all three possible labels even when a verifier did not start.
    """
    evidence = Path(evidence)
    execution_id = evidence.name
    if not re.fullmatch(r'planar-t044-[0-9a-f]{32}', execution_id):
        raise ValueError('invalid execution identity')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', trial_name):
        raise ValueError('invalid trial name')
    steps = trial.get('step_results')
    if not isinstance(steps, list) or len(steps) != 2:
        raise ValueError('native step coverage unavailable')
    by_name = {}
    for step in steps:
        if not isinstance(step, dict) or step.get('step_name') not in ('worker', 'review') or step['step_name'] in by_name:
            raise ValueError('native step coverage ambiguous')
        by_name[step['step_name']] = step
    prefix = (execution_id + '-' + trial_name).lower()
    names = {prefix+'__env', prefix+'__verifier__worker', prefix+'__verifier__review'}
    required = {prefix+'__env'}
    for name, step in by_name.items():
        if 'verifier' not in step:
            raise ValueError('native verifier state unavailable')
        verifier = step['verifier']
        if verifier is not None:
            if not isinstance(verifier, dict) or not isinstance(verifier.get('started_at'), str):
                raise ValueError('native verifier start ambiguous')
            required.add(prefix+'__verifier__'+name)
    for name in ('worker', 'review'):
        receipt = evidence/('verifier-compose-up-'+name+'.json')
        if receipt.is_symlink():
            raise ValueError('unsafe verifier receipt')
        if receipt.exists():
            if not receipt.is_file() or receipt.stat().st_nlink != 1:
                raise ValueError('unsafe verifier receipt')
            obj = json.loads(receipt.read_bytes(), object_pairs_hook=unique)
            if obj != {'execution_id': execution_id, 'command': ['up','--detach','--wait','--no-build','--pull','never']}:
                raise ValueError('verifier receipt mismatch')
            required.add(prefix+'__verifier__'+name)
        elif prefix+'__verifier__'+name in required:
            raise ValueError('native verifier start lacks receipt')
    registry = evidence/'projects'
    if registry.is_symlink() or not registry.is_dir():
        raise ValueError('project registry unavailable')
    observed = set()
    for path in registry.iterdir():
        if path.is_symlink() or not path.is_file() or path.stat().st_nlink != 1:
            raise ValueError('unsafe project record')
        obj = json.loads(path.read_bytes(), object_pairs_hook=unique)
        if not isinstance(obj, dict) or set(obj) != {'execution_id', 'compose_project'}:
            raise ValueError('project record malformed')
        name = obj['compose_project']
        if (obj['execution_id'] != execution_id or not isinstance(name, str) or name not in names or name in observed
            or path.name != hashlib.sha256(name.encode()).hexdigest()+'.json'):
            raise ValueError('project record identity mismatch')
        observed.add(name)
    if not required <= observed:
        raise ValueError('started native environment lacks registry record')
    for name in sorted(names):
        for cmd in (['container','ls','-aq'], ['network','ls','-q'], ['volume','ls','-q']):
            result = run(['docker', *cmd, '--filter', 'label=com.docker.compose.project='+name], capture_output=True, text=True, timeout=15)
            if result.returncode or result.stdout.strip():
                raise ValueError('owned resource absence unproven')
    return {'registered_projects': sorted(observed), 'queried_projects': sorted(names)}
