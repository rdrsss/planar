"""Read the host-owned first-step verifier result for this selected case.

Call only after Harbor returns to the second fresh instruction. This check does
not itself collect or authenticate a snapshot; callers must revalidate it.
"""
import json
from pathlib import Path
import re


def worker_gate(evidence):
    root = Path(evidence)
    if root.is_symlink() or not root.is_dir(): raise ValueError('unsafe evidence root')
    start = root/'native-trial-start.json'
    if start.is_symlink() or not start.is_file(): raise ValueError('trial identity missing')
    data = json.loads(start.read_text())
    trial = data.get('trial_name')
    if not isinstance(trial, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,160}', trial):
        raise ValueError('invalid native trial identity')
    expected_job = root/root.name
    if data.get('job_dir') != str(expected_job): raise ValueError('native job identity mismatch')
    path = expected_job/trial/'steps/worker/verifier/reward.txt'
    for ancestor in (expected_job, expected_job/trial, expected_job/trial/'steps', expected_job/trial/'steps/worker', path.parent, path):
        if ancestor.is_symlink(): raise ValueError('unsafe gate path')
    if not path.is_file(): raise ValueError('first-step gate missing')
    if path.stat().st_size > 32 or path.read_bytes().strip() not in (b'1', b'1.0'):
        raise ValueError('first-step gate did not pass')
    return {'trial_name':trial, 'reward':'1'}
