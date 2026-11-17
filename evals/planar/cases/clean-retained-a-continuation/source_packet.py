"""Closed source evidence for the explicitly limited clean continuation."""
import hashlib
import json
import os
from pathlib import Path
import stat

POLICY = Path(__file__).with_name('source-policy.json')
MAX_FILE = 4 * 1024 * 1024
MAX_TOTAL = 16 * 1024 * 1024
AUDIT = '21e05ed1fd27ddb8430bf8805460b8cdf10cdd9bd996f69175e112df44d7d47a'


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def validate_closed(packet, candidate_inventory):
    """Validate exact frozen files and facts; caller independently checks image assessment."""
    packet = Path(packet)
    if packet.is_symlink() or not packet.is_dir(): raise ValueError('unsafe source packet')
    policy = json.loads(POLICY.read_bytes())
    if (policy.get('schema') != 'clean-retained-a-source-v1' or
        policy.get('worker_role_delivery') != 'unverified' or
        policy.get('source_semantics') != 'packet-read-causal-write-completion-only' or
        policy.get('source_online_status') != 'behavioral_failure' or
        type(policy.get('source_online_reward')) is not int or policy['source_online_reward'] != 0 or
        policy.get('files', {}).get('evidence/semantic-audit.md', {}).get('sha256') != AUDIT):
        raise ValueError('source limitations changed')
    if candidate_inventory != policy['candidate_sha256']: raise ValueError('candidate mismatch')
    expected=policy['files']; found={}
    allowed={str(parent) for name in expected for parent in Path(name).parents if str(parent) != '.'}
    for root, dirs, files in os.walk(packet, followlinks=False):
        root=Path(root)
        for name in dirs:
            path=root/name
            if path.is_symlink() or not path.is_dir() or path.relative_to(packet).as_posix() not in allowed:
                raise ValueError('unsafe packet directory')
        for name in files:
            path=root/name; key=path.relative_to(packet).as_posix(); info=path.lstat()
            if key not in expected or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > MAX_FILE:
                raise ValueError('unsafe packet file')
            fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
            with os.fdopen(fd,'rb') as stream: data=stream.read(MAX_FILE+1)
            found[key]={'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            if found[key] != expected[key]: raise ValueError('source evidence drift')
    if set(found) != set(expected) or sum(x['size'] for x in found.values()) > MAX_TOTAL:
        raise ValueError('source packet inventory mismatch')
    def read(name): return json.loads((packet/'evidence'/name).read_bytes())
    gate=read('original-gate.json'); launcher=read('launcher.json'); native=read('native-trial-start.json')
    job=read('native-job-result.json'); trial=read('native-trial-result.json'); regrade=read('offline-regrade.json')
    if any(value.get('execution_id') != policy['execution'] for value in (gate, launcher)):
        raise ValueError('source execution mismatch')
    if (native.get('job_id') != policy['job'] or native.get('trial_id') != policy['trial'] or
        job.get('id') != policy['job'] or trial.get('id') != policy['trial'] or
        trial.get('config',{}).get('job_id') != policy['job'] or
        type(job.get('n_total_trials')) is not int or job['n_total_trials'] != 1):
        raise ValueError('source native identity mismatch')
    resolution=read('original-resolution.json'); release=read('original-resolution-review.json')
    census=read('original-resolution-census.json')
    if (resolution.get('schema') != 'planar-clean-work-account-marker-v1' or
        resolution.get('type') != 'clean-work-account-v1' or resolution.get('execution_id') != policy['execution'] or
        resolution.get('review_sha256') != found['evidence/original-resolution-review.json']['sha256'] or
        hashlib.sha256(canonical(census)).hexdigest() != resolution.get('census_sha256') or
        resolution.get('review_binding') != release or release.get('execution_id') != policy['execution'] or
        release.get('terminal_class') != 'behavioral_failure' or release.get('decision') != 'release-account' or
        release.get('independent_review') is not True):
        raise ValueError('source account resolution mismatch')
    if read('original-status.json') != {'status':'behavioral_failure'} or (packet/'evidence/original-reward.txt').read_bytes() != b'0\n':
        raise ValueError('source outcome changed')
    if (regrade.get('original_online_outcome') != {'status':'behavioral_failure','reward':0} or
        regrade.get('offline_assessment',{}).get('status') != 'pass' or
        regrade['offline_assessment'].get('semantic_audit') != 'required' or
        regrade['offline_assessment'].get('lineage',{}).get('role_delivery') != 'unverified'):
        raise ValueError('source assessment changed')
    return {'source_packet_sha256':hashlib.sha256(canonical(policy)).hexdigest(),
            'worker_role_delivery':'unverified','source_online_status':'behavioral_failure',
            'source_online_reward':0,'semantic_scope':'packet-read-causal-write-completion-only'}


def validate(packet, candidate):
    """Reassess the bound source in its pinned isolated verifier image."""
    import subprocess
    from candidate import inventory
    packet=Path(packet)
    binding=validate_closed(packet, inventory(Path(candidate))['sha256'])
    policy=json.loads(POLICY.read_bytes())
    result=subprocess.run(['docker','run','--rm','--pull','never','--network','none','--read-only',
        '--cap-drop','ALL','--security-opt','no-new-privileges','--mount',
        f'type=bind,src={packet.resolve() / "source"},dst=/evidence,readonly',
        policy['assessor_image'],'python3','-c',
        'import json,sys; sys.path.insert(0,"/opt/planar-clean"); from verify_a import assess; print(json.dumps(assess("/evidence")))'],
        capture_output=True,text=True,check=True,timeout=60)
    assessed=json.loads(result.stdout)
    expected=json.loads((packet/'evidence/offline-regrade.json').read_bytes())['offline_assessment']
    if assessed != expected: raise ValueError('source offline reassessment mismatch')
    # Prevent mutable host evidence from changing during the assessment.
    if validate_closed(packet,inventory(Path(candidate))['sha256']) != binding:
        raise ValueError('source evidence changed during assessment')
    return binding
