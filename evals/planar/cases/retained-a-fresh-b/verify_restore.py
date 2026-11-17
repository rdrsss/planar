"""Two-root restoration and sole-injection checks; never issues a B reward."""
import hashlib
import json
from pathlib import Path

from source_packet import validate, validate_inprocess


def _verify(packet, target, binding):
    from capture import load
    from chronology import receipt, order
    from boundary import verify_injection
    packet, target = Path(packet), Path(target)
    source = load(packet/'source/pre')
    restored = load(target/'restored-pre'); post = load(target/'post')
    source_time = receipt(packet/'source','pre')
    restored_time, post_time = receipt(target,'restored-pre'), receipt(target,'post')
    order(source_time, restored_time)
    injection_path = target/'injection.json'
    if injection_path.is_symlink() or not injection_path.is_file(): raise ValueError('injection receipt missing')
    injection = json.loads(injection_path.read_text())
    order(restored_time, injection['started']); order(injection['started'], injection['ended']); order(injection['ended'], post_time)
    if set(source) != set(restored): raise ValueError('restored inventory mismatch')
    durable = {name for name in source if name.startswith('home/') or name in ('repository/README.md','repository/RESULT.txt','repository/STATE.txt')}
    restored_durable = {name for name in restored if name.startswith('home/') or name in ('repository/README.md','repository/RESULT.txt','repository/STATE.txt')}
    if durable != restored_durable or any(source[name] != restored[name] for name in durable):
        raise ValueError('restored durable source mismatch')
    for name in ('host/git-head.txt','host/git-stage.txt','host/git-status.txt'):
        if source.get(name) != restored.get(name): raise ValueError('restored git identity mismatch')
    old = json.loads(restored['host/repository-inventory.json']); new = json.loads(post['host/repository-inventory.json'])
    before = old['files'].pop('STATE.txt',None); after = new['files'].pop('STATE.txt',None)
    def meta(data): return {'sha256':hashlib.sha256(data).hexdigest(),'size':len(data)}
    if before != meta(restored['repository/STATE.txt']) or after != meta(post['repository/STATE.txt']) or old != new:
        raise ValueError('injection inventory mismatch')
    actual = verify_injection({k:v for k,v in restored.items() if k != 'host/repository-inventory.json'},
                              {k:v for k,v in post.items() if k != 'host/repository-inventory.json'})
    if injection.get('change') != actual: raise ValueError('injection receipt mismatch')
    return {'source_packet_sha256':binding['source_packet_sha256'], 'boundary':'restored-and-injected',
            'target_b_assessment':'not-implemented'}


def verify(packet, candidate, target):
    return _verify(packet, target, validate(packet, candidate))


def verify_inprocess(packet, candidate, target):
    return _verify(packet, target, validate_inprocess(packet, candidate))
