"""Case-selected host module loader; no authority to launch a trial."""
import importlib.util
from pathlib import Path
import sys


def load_case_module(case, name):
    if name not in ('capture', 'step_identity', 'handoff'):
        raise ValueError('unsupported clean-revision module')
    root = Path(case).resolve(strict=True)
    path = root/(name+'.py')
    if path.is_symlink() or not path.is_file(): raise ValueError('unsafe case module')
    spec = importlib.util.spec_from_file_location('_planar_clean_'+name, path)
    if spec is None or spec.loader is None: raise ValueError('case module unavailable')
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(root))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(root))
    return module


def check_revision_time(evidence, minimum_seconds, *, monotonic_ns=None, now=None):
    """Fail closed when the existing job or reviewed expiry lacks B's budget."""
    from datetime import datetime, timezone
    import json
    import os
    import time

    root = Path(evidence)
    record = root/'harbor-process.json'
    if root.is_symlink() or record.is_symlink() or not record.is_file():
        raise ValueError('trusted watchdog start is unavailable')
    try:
        data = json.loads(record.read_text())
        limit = data['deadline_seconds']
        started = data['started_monotonic_ns']
        absolute = os.environ['PLANAR_T044_ABSOLUTE_DEADLINE']
        expiry = datetime.fromisoformat(absolute)
        if (data['execution_id'] != root.name or data['absolute_deadline'] != absolute
            or os.environ.get('PLANAR_T044_DEADLINE_SECONDS') != '1050'
            or type(started) is not int or started <= 0
            or type(limit) not in (int, float) or not 0 < limit <= 1050
            or type(minimum_seconds) not in (int, float) or minimum_seconds <= 0
            or expiry.tzinfo is None):
            raise ValueError()
        current = time.monotonic_ns() if monotonic_ns is None else monotonic_ns
        wall = datetime.now(timezone.utc) if now is None else now
        if type(current) is not int or current < started or wall.tzinfo is None:
            raise ValueError()
        job_remaining = limit - (current-started)/1_000_000_000
        expiry_remaining = (expiry-wall).total_seconds()
    except (KeyError, TypeError, ValueError, OverflowError):
        raise ValueError('invalid watchdog budget or reviewed expiry') from None
    if job_remaining < minimum_seconds or expiry_remaining < minimum_seconds:
        raise TimeoutError('insufficient time for revision, verifier and cleanup')
    return {'job_remaining':job_remaining, 'expiry_remaining':expiry_remaining}
