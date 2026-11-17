"""Closed no-fault boundary for one clean retained-A continuation.

The host supplies independently collected, quiescent source and target bytes.
No capture, account admission, source audit or behavioral grade is implied.
"""

BASE = b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n'
PRODUCTS = {'repository/RESULT.txt': b'2\n', 'repository/STATE.txt': b'correct\n'}
GIT = ('host/git-head.txt', 'host/git-stage.txt', 'host/git-status.txt')


def _durable(files):
    return {name for name in files if name.startswith('home/') or name in
            ('repository/README.md', 'repository/RESULT.txt', 'repository/STATE.txt')}


def verify(source, restored, post, *, source_receipt, restored_receipt, post_receipt,
           source_job, target_job, injection_exists):
    """Check exact handoff bytes; callers must separately validate each capture."""
    if not all(isinstance(x, dict) for x in (source, restored, post)):
        raise ValueError('snapshot unavailable')
    if not isinstance(source_job, str) or not source_job or not isinstance(target_job, str) or not target_job or source_job == target_job:
        raise ValueError('source and target provenance must differ')
    if type(injection_exists) is not bool or injection_exists:
        raise ValueError('injection evidence is forbidden')
    for receipt in (source_receipt, restored_receipt, post_receipt):
        if not isinstance(receipt, dict) or type(receipt.get('wall_ns')) is not int or receipt['wall_ns'] <= 0:
            raise ValueError('capture time unavailable')
    if not (source_receipt['wall_ns'] < restored_receipt['wall_ns'] < post_receipt['wall_ns']):
        raise ValueError('capture chronology invalid')
    if any(type(receipt.get('monotonic_ns')) is not int or receipt['monotonic_ns'] <= 0
           for receipt in (restored_receipt, post_receipt)) or restored_receipt['monotonic_ns'] >= post_receipt['monotonic_ns']:
        raise ValueError('target monotonic chronology invalid')
    for name, value in PRODUCTS.items():
        if any(snapshot.get(name) != value for snapshot in (source, restored, post)):
            raise ValueError('product boundary changed')
    if any(snapshot.get('host/git-head.txt') != BASE for snapshot in (source, restored, post)):
        raise ValueError('base boundary changed')
    if _durable(source) != _durable(restored) or any(source[name] != restored[name] for name in _durable(source)):
        raise ValueError('durable restore differs from source')
    if any(source.get(name) is None or source[name] != restored.get(name) for name in GIT):
        raise ValueError('rebuilt Git boundary differs from source')
    if restored != post:
        raise ValueError('no-fault handoff changed')
    return {'boundary': 'restored-no-fault', 'source_job': source_job,
            'target_job': target_job, 'semantic_audit': 'required'}


async def handoff(environment, capture, *, container_id, baseline, source, target,
                  source_receipt, source_job, target_job):
    """Capture a host-owned, quiescent post boundary without mutating guest files."""
    import json
    from pathlib import Path
    target=Path(target)
    injection=target/'injection.json'
    if injection.exists() or injection.is_symlink(): raise ValueError('injection evidence is forbidden')
    restored=capture.load(target/'restored-pre')
    before=json.loads((target/'restored-pre-receipt.json').read_bytes())
    post=await capture.collect(environment,container_id,baseline,failure_evidence=target)
    if restored != post: raise ValueError('no-fault handoff changed')
    capture.save(target/'post',post)
    after=json.loads((target/'post-receipt.json').read_bytes())
    return verify(source,restored,post,source_receipt=source_receipt,restored_receipt=before,
                  post_receipt=after,source_job=source_job,target_job=target_job,
                  injection_exists=injection.exists() or injection.is_symlink())
