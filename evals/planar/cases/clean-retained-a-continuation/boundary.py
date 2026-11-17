"""Compare trusted, already-collected boundary snapshots for this fixture.

Collection, process quiescence and authenticated host ownership are separate
preconditions; a mapping supplied by the agent is not trustworthy evidence.
"""
import hashlib

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

