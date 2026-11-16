"""Separate verifier for the bounded native smoke; full acceptance is pending."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys

BUNDLE = Path(__file__).resolve().parent / 'bundle'
sys.path.insert(0, str(BUNDLE))
try:
    spec = importlib.util.spec_from_file_location('online_regrade_dispatch', BUNDLE / 'regrade_dispatch.py')
    grader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(grader)
finally:
    sys.path.remove(str(BUNDLE))
FILES, events, grade_rollouts, one_meta, read_regular = (grader.FILES, grader.events, grader.grade_rollouts, grader.one_meta, grader.read_regular)

FROZEN = '2e229df68784143220d6550bed33719016b1547689ac5a25d5d9f1e4cb1f3368'


def result_check(root: Path):
    if root.is_symlink() or not root.is_dir():
        return 'invalid fixture root'
    for item in root.iterdir():
        if item.name != 'RESULT.txt' or item.is_symlink():
            return 'unexpected fixture entry'
    try:
        fd = os.open(root / 'RESULT.txt', os.O_RDONLY | os.O_NOFOLLOW)
    except OSError:
        return 'missing or invalid result'
    with os.fdopen(fd, 'rb') as file:
        if not stat.S_ISREG(os.fstat(file.fileno()).st_mode) or file.read(3) != b'2\n':
            return 'wrong result bytes'
    return None


def expected_files(bundle):
    manifest = json.loads(read_regular(bundle / 'manifest.json'))
    entries = manifest['files']
    canonical = json.dumps(entries, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()
    if manifest['algorithm'] != 'sha256-json-inventory-v1' or manifest['sha256'] != FROZEN or hashlib.sha256(canonical).hexdigest() != FROZEN:
        raise ValueError('frozen manifest mismatch')
    hashes = {entry['path']: entry['sha256'] for entry in entries}
    if len(hashes) != len(entries):
        raise ValueError('duplicate manifest entry')
    result = {}
    for name in FILES:
        path = bundle / 'candidate' / name
        if any(node.is_symlink() for node in (path, *list(path.parents)[:len(Path(name).parts)])):
            raise ValueError('unsafe candidate reference')
        raw = read_regular(path)
        if hashlib.sha256(raw).hexdigest() != hashes[name]:
            raise ValueError('frozen reference mismatch')
        result[name] = raw.decode('utf-8')
    return result


def assess(workspace=Path('/workspace'), agent=Path('/logs/agent'), bundle=BUNDLE):
    error = result_check(workspace)
    if error:
        return {'status': 'behavioral_failure', 'reason': error}
    if agent.is_symlink() or not agent.is_dir() or (agent / 'sessions').is_symlink():
        raise ValueError('missing or unsafe agent sessions')
    paths = list((agent / 'sessions').rglob('rollout-*.jsonl'))
    if len(paths) != 2 or any(any(node.is_symlink() for node in [path, *list(path.parents)[:4]]) for path in paths):
        raise ValueError('expected exactly two safe rollouts')
    rollouts = [events(path) for path in paths]
    parents = [ev for ev in rollouts if one_meta(ev).get('source') == 'exec']
    if len(parents) != 1:
        raise ValueError('root rollout not unique')
    child = next(ev for ev in rollouts if ev is not parents[0])
    dimensions = grade_rollouts(parents[0], child, expected_files(bundle))
    dimensions.update({'result_bytes': 'pass', 'host_provenance': 'unverified', 'full_test_s69': 'pending'})
    return {'scope': 'online-narrow-native-dispatch', 'status': 'infrastructure_error',
            'reason': 'full TEST-S69 remains pending', 'dimensions': dimensions,
            'limitations': ['native events are agent-writable', 'encrypted role delivery is unverified',
                            'backend model and effort identity are unverified',
                            'host provenance is unavailable in the separate verifier',
                            'skill inventory does not prove automatic activation',
                            'tool output does not independently prove command exit status']}


def main():
    try:
        result = assess()
    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
        result = {'status': 'infrastructure_error', 'reason': str(exc)}
    print(json.dumps(result, sort_keys=True))
    return 1 if result['status'] == 'behavioral_failure' else 2


if __name__ == '__main__':
    sys.exit(main())
