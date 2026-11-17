"""Compare trusted, already-collected boundary snapshots for this fixture.

Collection, process quiescence and authenticated host ownership are separate
preconditions; a mapping supplied by the agent is not trustworthy evidence.
"""
import hashlib

STATE = 'repository/STATE.txt'
BEFORE = b'correct\n'
AFTER = b'fault-injected\n'


def manifest(snapshot):
    if not isinstance(snapshot, dict) or not snapshot:
        raise ValueError('snapshot missing')
    result = {}
    for name, content in snapshot.items():
        if (not isinstance(name, str) or name.startswith('/') or
            any(p in ('', '.', '..') for p in name.split('/')) or
            not isinstance(content, bytes)):
            raise ValueError('invalid snapshot entry')
        result[name] = {'sha256': hashlib.sha256(content).hexdigest(), 'size': len(content)}
    return result


def verify_injection(before, after):
    """Require identical inventories and bytes except for the fixed STATE fault."""
    pre, post = manifest(before), manifest(after)
    if set(pre) != set(post):
        raise ValueError('snapshot inventory changed')
    if before.get(STATE) != BEFORE or after.get(STATE) != AFTER:
        raise ValueError('STATE transition mismatch')
    if any(before[name] != after[name] for name in before if name != STATE):
        raise ValueError('unexpected injection delta')
    return {'changed': STATE, 'before': pre[STATE], 'after': post[STATE]}
