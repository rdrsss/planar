"""Read clean-work verifier outputs without interpreting task success."""
import hashlib
import json
from pathlib import Path

from retained_b_early_evidence import read_safe
from terminal_failure_schema import unique


def observe(trial_dir, evidence_root, trial):
    trial_dir, evidence_root = Path(trial_dir), Path(evidence_root)
    steps = trial.get('step_results')
    if not isinstance(steps, list) or len(steps) != 2:
        raise ValueError('unsupported native step result shape')
    mapped = {}
    for step in steps:
        if not isinstance(step, dict) or step.get('step_name') not in ('worker','review') or step['step_name'] in mapped:
            raise ValueError('ambiguous native step results')
        mapped[step['step_name']] = step
    observed = {}
    for name in ('worker','review'):
        if 'exception_info' not in mapped[name]: raise ValueError('native step exception unavailable')
        native_exception=mapped[name]['exception_info']
        if native_exception is not None and not isinstance(native_exception,dict): raise ValueError('native step exception malformed')
        base = trial_dir/'steps'/name/'verifier'
        status, reward = base/'status.json', base/'reward.txt'
        for path in (status,reward):
            if path.is_symlink(): raise ValueError('unsafe verifier output')
        status_exists, reward_exists = status.exists(), reward.exists()
        if status_exists != reward_exists:
            raise ValueError('incomplete verifier output pair')
        native_verifier=mapped[name].get('verifier_result')
        if 'verifier_result' not in mapped[name]: raise ValueError('native verifier result unavailable')
        if not status_exists:
            if native_verifier is not None: raise ValueError('native verifier output mismatch')
            observed[name] = None
            continue
        status_raw = read_safe(status,evidence_root)
        reward_raw = read_safe(reward,evidence_root)
        result = json.loads(status_raw,object_pairs_hook=unique)
        if not isinstance(result,dict) or result.get('status') not in ('pass','incomplete','behavioral_failure'):
            raise ValueError('unsupported verifier status')
        if 'reason' in result and (type(result['reason']) is not str or not result['reason']):
            raise ValueError('invalid verifier reason')
        if reward_raw not in (b'0\n', b'1\n'):
            raise ValueError('unsupported verifier reward')
        if not isinstance(native_verifier,dict) or set(native_verifier)!={'rewards'} or not isinstance(native_verifier['rewards'],dict) or set(native_verifier['rewards'])!={'reward'}:
            raise ValueError('native verifier result malformed')
        if (result['status']=='pass') != (reward_raw==b'1\n'):
            raise ValueError('verifier status and reward mismatch')
        native_reward=native_verifier['rewards']['reward']
        if type(native_reward) not in (int,float) or native_reward!=int(reward_raw):
            raise ValueError('native verifier reward mismatch')
        observed[name] = {'status':result['status'], 'reason':result.get('reason'),
                          'reward':int(reward_raw), 'exception':native_exception, 'status_sha256':hashlib.sha256(status_raw).hexdigest(),
                          'reward_sha256':hashlib.sha256(reward_raw).hexdigest()}
    return observed
