"""Recorded native lineage checks for this one reviewer; not hostile attestation."""
import json
import re
from datetime import datetime

class MissingEvidence(ValueError): pass
class InvalidBehavior(ValueError): pass


def _time(event):
    try:
        value = datetime.fromisoformat(event['timestamp'].replace('Z', '+00:00'))
        if value.tzinfo is None: raise ValueError()
        return value
    except (KeyError, TypeError, ValueError, AttributeError):
        raise MissingEvidence('invalid timestamp') from None


def _meta(rows):
    found = [e['payload'] for e in rows if isinstance(e, dict) and e.get('type') == 'session_meta' and isinstance(e.get('payload'), dict)]
    if len(found) != 1 or not isinstance(found[0].get('id'), str): raise MissingEvidence('session metadata missing or ambiguous')
    return found[0]


def correlate(parent, child):
    """Check structured records; callers must validate and safely collect all rollouts."""
    if not isinstance(parent, list) or not isinstance(child, list): raise MissingEvidence('invalid rollout')
    pm, cm = _meta(parent), _meta(child)
    pid, cid = pm['id'], cm['id']
    if not pid or not cid or pid == cid or pm.get('source') != 'exec' or pm.get('originator') != 'codex_exec': raise InvalidBehavior('root identity mismatch')
    source = cm.get('source')
    sub = source.get('subagent') if isinstance(source, dict) else None
    spawn = sub.get('thread_spawn') if isinstance(sub, dict) else None
    if not isinstance(spawn, dict): raise MissingEvidence('spawn metadata missing')
    path = spawn.get('agent_path')
    if spawn.get('parent_thread_id') != pid or cm.get('parent_thread_id') != pid or spawn.get('depth') != 1 or not isinstance(path, str) or cm.get('agent_path') != path: raise InvalidBehavior('child lineage mismatch')
    calls, outputs = {}, {}
    for i, e in enumerate(parent):
        if not isinstance(e, dict) or not isinstance(e.get('payload'), dict): raise MissingEvidence('malformed event')
        p = e['payload']
        if e.get('type') != 'response_item' or p.get('type') not in ('function_call', 'function_call_output'): continue
        target = calls if p['type'] == 'function_call' else outputs
        key = p.get('call_id')
        if not isinstance(key, str) or key in target: raise MissingEvidence('ambiguous call identity')
        target[key] = (i, e)
    spawns = [(k, i, e) for k, (i, e) in calls.items() if e['payload'].get('namespace') == 'collaboration' and e['payload'].get('name') == 'spawn_agent']
    if len(spawns) != 1: raise InvalidBehavior('expected exactly one child dispatch')
    key, idx, event = spawns[0]
    try: args = json.loads(event['payload']['arguments'])
    except (KeyError, TypeError, json.JSONDecodeError): raise MissingEvidence('malformed spawn') from None
    if not isinstance(args, dict): raise MissingEvidence('malformed spawn')
    task = args.get('task_name')
    if args.get('fork_turns') != 'none' or args.get('model') != 'gpt-6.1-sol' or args.get('reasoning_effort') != 'medium' or not isinstance(task, str) or not re.fullmatch(r'[a-z0-9_]+', task) or path != '/root/' + task: raise InvalidBehavior('reviewer freshness, path or selected pins mismatch')
    if key not in outputs: raise MissingEvidence('spawn output missing')
    oi, out = outputs[key]
    raw = out['payload'].get('output')
    if isinstance(raw, list) and all(isinstance(x, dict) and isinstance(x.get('text'), str) for x in raw): raw = ''.join(x['text'] for x in raw)
    try: result = json.loads(raw)
    except (TypeError, json.JSONDecodeError): raise MissingEvidence('malformed spawn output') from None
    if not isinstance(result, dict) or result.get('task_name') != path: raise InvalidBehavior('spawn output mismatch')
    child_meta = next(e for e in child if e.get('type') == 'session_meta')
    if oi <= idx or not (_time(event) <= _time(child_meta) <= _time(out)): raise InvalidBehavior('spawn order mismatch')
    completion = []
    for rows in (parent, child):
        if any(not isinstance(e, dict) or not isinstance(e.get('payload'), dict) for e in rows): raise MissingEvidence('malformed event')
        found = [(i, e) for i, e in enumerate(rows) if e.get('type') == 'event_msg' and e['payload'].get('type') == 'task_complete']
        if len(found) != 1: raise MissingEvidence('completion missing or ambiguous')
        completion.append(found[0])
    (pi, pe), (ci, ce) = completion
    if pi <= oi or ci <= child.index(child_meta) or _time(pe) < _time(ce) or _time(ce) < _time(child_meta): raise InvalidBehavior('completion order mismatch')
    return {'parent_id':pid, 'child_id':cid, 'agent_path':path, 'selected_model':'gpt-6.1-sol', 'selected_effort':'medium', 'backend_identity':'unavailable', 'role_delivery':'unverified', 'causal_write':'unverified'}
