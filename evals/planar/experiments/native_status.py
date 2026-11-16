"""Classify native typed errors and separately verified structured evidence.

Never interpret model/tool text. Behavioral grades require pinned case verifier
artifacts, a valid native reward and an explicit matching structured status.
"""
import json
from pathlib import Path


def classify(result):
    names = {error.exception_type for error in result.recorded_exceptions()}
    if 'ApiUsageLimitError' in names:
        return {'status': 'incomplete', 'reason': 'native usage limit', 'stop_account': True}
    if 'ApiRateLimitError' in names:
        return {'status': 'incomplete', 'reason': 'native rate limit', 'stop_account': True}
    if names & {'AgentAuthenticationError', 'AuthenticationError', 'PermissionDeniedError'}:
        return {'status': 'error', 'reason': 'native authentication error', 'stop_account': True}
    if names & {'AgentTimeoutError', 'VerifierTimeoutError', 'EnvironmentStartTimeoutError', 'AgentSetupTimeoutError'}:
        return {'status': 'incomplete', 'reason': 'native timeout', 'stop_account': False}
    if names:
        return {'status': 'error', 'reason': 'native infrastructure or unknown exception', 'stop_account': True}
    return {'status': 'incomplete', 'reason': 'independent verifier assessment required', 'stop_account': False}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate verifier status key')
        result[key] = value
    return result


def assess_verified(result, verifier_dir):
    """Use only native structured reward and matching separate-verifier status.

    Caller must first establish identity, case pinning and cleanup. Current
    supported cases write one required binary reward plus status.json; a zero
    also represents verifier infrastructure failure, so status must agree.
    """
    outcome = classify(result)
    if result.recorded_exceptions():
        return outcome
    try:
        directory = Path(verifier_dir)
        status_file, reward_file = directory / 'status.json', directory / 'reward.txt'
        if (directory.is_symlink() or not directory.is_dir() or status_file.is_symlink()
            or reward_file.is_symlink() or not status_file.is_file() or not reward_file.is_file()
            or (directory / 'reward.json').exists() or status_file.stat().st_size > 65536
            or reward_file.stat().st_size > 128):
            raise ValueError('missing, unsafe or conflicting verifier artifacts')
        if (result.config.verifier.disable or result.verifier_environment_mode != 'separate'
            or result.step_results or not result.reward_valid or result.verifier is None
            or result.verifier.started_at is None or result.verifier.finished_at is None
            or not (result.started_at <= result.verifier.started_at <= result.verifier.finished_at <= result.finished_at)):
            raise ValueError('separate verifier execution unproven')
        status = json.loads(status_file.read_text(), object_pairs_hook=_unique_object)
        reward = result.verifier_result.rewards
        raw = reward_file.read_text().strip()
        if not isinstance(status, dict) or not isinstance(reward, dict) or set(reward) != {'reward'} or raw not in ('0', '1'):
            raise ValueError('missing or unsupported required reward')
        value = reward['reward']
        if type(value) not in (int, float) or value not in (0, 1) or value != int(raw):
            raise ValueError('native and retained reward disagree')
        expected = 'pass' if value == 1 else 'behavioral_failure'
        if status.get('status') != expected:
            raise ValueError('verifier status and reward disagree')
        return {'status': expected, 'reason': 'separate verifier structured reward and status agree', 'stop_account': False}
    except (OSError, UnicodeError, ValueError, TypeError, AttributeError, json.JSONDecodeError):
        return {'status': 'incomplete', 'reason': 'missing, invalid or contradictory verifier evidence', 'stop_account': False}
