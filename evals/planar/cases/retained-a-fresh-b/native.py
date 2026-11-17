"""Bounded two-parent native lineage checks; not hostile attestation."""
import json
import re
from datetime import datetime

class MissingEvidence(ValueError): pass
class InvalidBehavior(ValueError): pass


def _time(event):
    try:
        result = datetime.fromisoformat(event['timestamp'].replace('Z', '+00:00'))
        if result.tzinfo is None:
            raise ValueError()
        return result
    except (KeyError, TypeError, ValueError, AttributeError):
        raise MissingEvidence('invalid timestamp') from None


def _meta(rows):
    if not isinstance(rows, list) or any(not isinstance(x, dict) or not isinstance(x.get('payload'), dict) for x in rows):
        raise MissingEvidence('invalid rollout')
    metas = [x for x in rows if x.get('type') == 'session_meta']
    if len(metas) != 1 or not isinstance(metas[0]['payload'].get('id'), str) or not metas[0]['payload']['id']:
        raise MissingEvidence('missing or ambiguous session identity')
    return metas[0]


def _completion(rows):
    done = [x for x in rows if x.get('type') == 'event_msg' and x['payload'].get('type') == 'task_complete']
    if len(done) != 1:
        raise MissingEvidence('missing or ambiguous completion')
    return done[0]


def _correlate(parents, children, patterns):
    """Correlate only the exact supplied fixture role pattern."""
    if not isinstance(parents, list) or not isinstance(children, list):
        raise MissingEvidence('rollout sets absent')
    expected_children = sum(len(group) for group in patterns)
    if len(parents) < len(patterns) or len(children) < expected_children:
        raise MissingEvidence('rollout missing')
    if len(parents) != len(patterns) or len(children) != expected_children:
        raise InvalidBehavior('unexpected rollout')
    pm = [_meta(p) for p in parents]
    cm = [_meta(c) for c in children]
    identities = [m['payload']['id'] for m in pm + cm]
    if len(set(identities)) != len(identities):
        raise InvalidBehavior('duplicate identity')
    selected = []
    offset = 0
    for parent, meta, roles in zip(parents, pm, patterns):
        pp = meta['payload']
        if pp.get('source') != 'exec' or pp.get('originator') != 'codex_exec':
            raise InvalidBehavior('parent identity mismatch')
        calls, outputs = {}, {}
        for i, e in enumerate(parent):
            p = e['payload']
            if e.get('type') != 'response_item' or p.get('type') not in ('function_call', 'function_call_output'):
                continue
            bucket = calls if p['type'] == 'function_call' else outputs
            key = p.get('call_id')
            if not isinstance(key, str) or key in bucket:
                raise MissingEvidence('ambiguous call identity')
            bucket[key] = (i, e)
        spawns = [(k, i, e) for k, (i, e) in calls.items() if e['payload'].get('namespace') == 'collaboration' and e['payload'].get('name') == 'spawn_agent']
        if len(spawns) < len(roles): raise MissingEvidence('spawn missing')
        if len(spawns) != len(roles): raise InvalidBehavior('unexpected dispatch')
        end = _completion(parent)
        prev = _time(meta)
        for (key, index, event), model, child, child_meta in zip(spawns, roles, children[offset:offset+len(roles)], cm[offset:offset+len(roles)]):
            try:
                args = json.loads(event['payload']['arguments'])
            except (KeyError, TypeError, json.JSONDecodeError):
                raise MissingEvidence('invalid spawn arguments') from None
            if not isinstance(args, dict): raise MissingEvidence('invalid spawn arguments')
            name = args.get('task_name')
            if args.get('fork_turns') != 'none' or args.get('model') != model or args.get('reasoning_effort') != 'medium' or not isinstance(name, str) or not re.fullmatch(r'[a-z0-9_]+', name):
                raise InvalidBehavior('spawn configuration mismatch')
            path = '/root/' + name
            if key not in outputs: raise MissingEvidence('spawn output missing')
            oi, out = outputs[key]
            raw = out['payload'].get('output')
            if isinstance(raw, list) and all(isinstance(x, dict) and isinstance(x.get('text'), str) for x in raw):
                raw = ''.join(x['text'] for x in raw)
            try: result = json.loads(raw)
            except (TypeError, json.JSONDecodeError): raise MissingEvidence('invalid spawn output') from None
            cp = child_meta['payload']
            source = cp.get('source')
            sub = source.get('subagent') if isinstance(source, dict) else None
            spawn = sub.get('thread_spawn') if isinstance(sub, dict) else None
            if not isinstance(spawn, dict): raise MissingEvidence('spawn metadata missing')
            if not isinstance(result, dict) or result.get('task_name') != path or cp.get('parent_thread_id') != pp['id'] or cp.get('agent_path') != path or spawn.get('parent_thread_id') != pp['id'] or spawn.get('agent_path') != path or spawn.get('depth') != 1:
                raise InvalidBehavior('child linkage mismatch')
            done = _completion(child)
            if (oi <= index or child.index(done) <= child.index(child_meta) or parent.index(end) <= oi
                or not (prev <= _time(event) <= _time(child_meta) <= _time(out) <= _time(end))
                or not (_time(child_meta) <= _time(done) <= _time(end))):
                raise InvalidBehavior('dispatch order mismatch')
            prev = _time(done)
            selected.append(cp['id'])
        offset += len(roles)
    if len(parents) == 2 and _time(_completion(parents[0])) >= _time(pm[1]):
        raise InvalidBehavior('parents overlap or out of order')
    return {'parents': identities[:len(parents)], 'children': selected, 'backend_identity': 'unavailable', 'role_delivery': 'unverified', 'read_order': 'unverified', 'causal_write': 'unverified'}


def correlate(parents, children):
    return _correlate(parents, children, [('gpt-6.1-terra',), ('gpt-6.1-sol', 'gpt-6.1-terra', 'gpt-6.1-sol')])


def correlate_first(parent, children):
    return _correlate([parent], children, [('gpt-6.1-terra',)])
