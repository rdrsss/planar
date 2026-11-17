"""Closed, fixed retained-A evidence: offline primitives, not a launcher."""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

POLICY = Path(__file__).with_name('source-policy.json')
MAX_FILE = 4 * 1024 * 1024
MAX_TOTAL = 16 * 1024 * 1024


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def _validate_closed(packet, candidate):
    """Check the fixed packet and candidate before any source assessment."""
    from candidate import inventory
    packet = Path(packet)
    if packet.is_symlink() or not packet.is_dir(): raise ValueError('unsafe packet root')
    policy = json.loads(POLICY.read_text())
    if policy['schema'] != 'retained-a-fresh-b-source-v1': raise ValueError('unsupported source policy')
    expected = policy['files']; found = {}
    allowed_dirs = {str(parent) for name in expected for parent in Path(name).parents if str(parent) != '.'}
    for root, dirs, files in os.walk(packet, followlinks=False):
        base = Path(root)
        for name in dirs:
            p = base/name
            if p.is_symlink() or not p.is_dir() or p.relative_to(packet).as_posix() not in allowed_dirs:
                raise ValueError('unsafe or extra packet directory')
        for name in files:
            p = base/name; key = p.relative_to(packet).as_posix(); info = p.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > MAX_FILE:
                raise ValueError('unsafe packet file')
            if key not in expected: raise ValueError('extra packet file')
            fd = os.open(p, os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(fd, 'rb') as f: data = f.read(MAX_FILE+1)
            found[key] = {'size':len(data), 'sha256':hashlib.sha256(data).hexdigest()}
            if found[key] != expected[key]: raise ValueError('source packet drift')
    if set(found) != set(expected) or sum(x['size'] for x in found.values()) > MAX_TOTAL:
        raise ValueError('source packet inventory mismatch')
    if inventory(Path(candidate))['sha256'] != policy['candidate_sha256']:
        raise ValueError('candidate mismatch')
    def read_json(name): return json.loads((packet/name).read_bytes())
    native = read_json('evidence/native-trial-start.json')
    if native['job_id'] != policy['job'] or native['trial_id'] != policy['trial']:
        raise ValueError('source native identity mismatch')
    archived_job = read_json('evidence/archived-job-result.json')
    archived_trial = read_json('evidence/archived-trial-result.json')
    if (archived_job.get('id') != policy['job'] or archived_job.get('n_total_trials') != 1 or
        archived_trial.get('id') != policy['trial'] or archived_trial.get('config',{}).get('job_id') != policy['job'] or
        archived_trial.get('trial_name') != native.get('trial_name') or
        archived_trial.get('verifier_result',{}).get('rewards',{}).get('reward') != 0.0):
        raise ValueError('archived source job or trial identity mismatch')
    gate = read_json('evidence/original-gate.json')
    launch = read_json('evidence/launcher.json')
    if gate.get('execution_id') != policy['execution'] or launch.get('execution_id') != policy['execution']:
        raise ValueError('source execution identity mismatch')
    if launch.get('job_config_sha256') != found['evidence/job-config.json']['sha256']:
        raise ValueError('source job configuration mismatch')
    for key in ('candidate_sha256','case_sha256','implementation_sha256','configuration_sha256'):
        if gate.get(key) != policy[key] or launch.get(key) != policy[key]: raise ValueError('source gate identity mismatch')
    if read_json('evidence/original-status.json').get('status') != policy['source_status'] or (packet/'evidence/original-reward.txt').read_text().strip() != '0':
        raise ValueError('source outcome mismatch')
    return policy


def _result(policy, assessment):
    if assessment.get('status') != 'pass' or assessment.get('semantic_audit') != 'required':
        raise ValueError('retained source A assessment failed')
    expected_parent = '01a1e776-1d33-71d3-9a40-cda0ac5b2f08'
    expected_child = '01a1e778-216b-7811-a3eb-5e095361b60d'
    if assessment.get('lineage',{}).get('parents') != [expected_parent] or assessment['lineage'].get('children') != [expected_child]:
        raise ValueError('source native lineage mismatch')
    return {'source_packet_sha256':hashlib.sha256(_canonical(policy)).hexdigest(), 'assessment':assessment}


def validate(packet, candidate):
    """Host-side source validation using the pinned, isolated A image."""
    policy = _validate_closed(packet, candidate); packet = Path(packet)
    result = subprocess.run(['docker','run','--rm','--pull','never','--network','none','--read-only',
        '--cap-drop','ALL','--security-opt','no-new-privileges','--mount',f'type=bind,src={packet.resolve() / "source"},dst=/evidence,readonly',
        policy['assessor_image'],'python3','-c',
        'import json,sys; sys.path.insert(0,"/opt/planar-clean"); from verify_a import assess; print(json.dumps(assess("/evidence")))'],
        capture_output=True,text=True,check=True,timeout=60)
    return _result(policy,json.loads(result.stdout))


def validate_inprocess(packet, candidate):
    """Separate-verifier-only A validation, with source oracle files rehashed.

    This uses no nested Docker and must run in the separately pinned verifier.
    Source logs stay in that verifier's host-owned evidence input, not B context.
    """
    import importlib
    policy = _validate_closed(packet, candidate)
    names=('verify_a','capture','chronology','native','boundary','snapshot','safe_export','assess_worker')
    modules={name:importlib.import_module(name) for name in names}
    for name,module in modules.items():
        path=Path(module.__file__)
        if path.is_symlink() or not path.is_file(): raise ValueError('unsafe verifier oracle')
        key='evidence/assess_worker.py' if name=='assess_worker' else 'evidence/oracle/'+name+'.py'
        if hashlib.sha256(path.read_bytes()).hexdigest()!=policy['files'][key]['sha256']:
            raise ValueError('verifier oracle drift')
    projection=Path(modules['verify_a'].__file__).with_name('projection.json')
    if projection.is_symlink() or hashlib.sha256(projection.read_bytes()).hexdigest()!=policy['files']['evidence/oracle/projection.json']['sha256']:
        raise ValueError('verifier projection drift')
    return _result(policy,modules['verify_a'].assess(Path(packet)/'source'))
