"""Preliminary fail-closed verifier; dispatch event verification is pending."""
from pathlib import Path
import json
import os
import stat
import sys


def result_check(root: Path):
    if root.is_symlink() or not root.is_dir():
        return 'invalid fixture root'
    allowed = {'RESULT.txt'}
    for item in root.iterdir():
        if item.name not in allowed or item.is_symlink():
            return 'unexpected fixture entry'
    result = root / 'RESULT.txt'
    try:
        fd = os.open(result, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError:
        return 'missing or invalid result'
    with os.fdopen(fd, 'rb') as file:
        if not stat.S_ISREG(os.fstat(file.fileno()).st_mode) or file.read(3) != b'2\n':
            return 'wrong result bytes'
    return None


def main():
    try:
        error = result_check(Path('/workspace'))
        if error:
            print(json.dumps({'status': 'behavioral_failure', 'reason': error}))
            return 1
        print(json.dumps({'status': 'infrastructure_error', 'reason': 'native dispatch evidence verifier is not implemented'}))
        return 2
    except Exception as exc:
        print(json.dumps({'status': 'infrastructure_error', 'reason': type(exc).__name__}))
        return 2


if __name__ == '__main__':
    sys.exit(main())

# The following narrow parser is a deterministic control only. Its accepted
# event envelope is based on Harbor 0.26.0's native rollout reader and has not
# been validated against a live Codex exec child. It is not wired to a pass.
def structural_dispatch(root_events, child_events, hashes):
    import hashlib
    if not isinstance(root_events, list) or not isinstance(child_events, list):
        return False
    def metadata(events):
        metas = [e.get('payload') for e in events if e.get('type') == 'session_meta']
        return metas[0] if len(metas) == 1 and isinstance(metas[0], dict) else {}
    root_meta, child_meta = metadata(root_events), metadata(child_events)
    root_id, child_id = root_meta.get('id'), child_meta.get('id')
    if not isinstance(root_id, str) or not isinstance(child_id, str) or not root_id or not child_id or root_id == child_id:
        return False
    source = child_meta.get('source', {})
    if not isinstance(source, dict):
        return False
    sub = source.get('subagent', {})
    spawn = sub.get('thread_spawn', {}) if isinstance(sub, dict) else {}
    if not isinstance(spawn, dict) or spawn.get('parent_thread_id') != root_id:
        return False
    reads = {}
    calls = {}
    dispatch = None
    dispatch_id = None
    spawn_output = None
    for event in root_events:
        if not isinstance(event, dict) or event.get('type') != 'response_item':
            continue
        payload = event.get('payload', {})
        if not isinstance(payload, dict):
            return False
        kind, call_id = payload.get('type'), payload.get('call_id')
        if kind in ('function_call', 'custom_tool_call') and isinstance(call_id, str):
            calls[call_id] = payload
            name = payload.get('name', '')
            if isinstance(name, str) and name.split('.')[-1] == 'spawn_agent':
                try:
                    args = json.loads(payload.get('arguments', ''))
                except (ValueError, TypeError):
                    return False
                if not isinstance(args, dict):
                    return False
                if dispatch is not None:
                    return False
                dispatch, dispatch_id = args, call_id
        elif kind in ('function_call_output', 'custom_tool_call_output') and call_id in calls:
            call = calls[call_id]
            if call_id == dispatch_id:
                try:
                    spawn_output = json.loads(payload.get('output', ''))
                except (ValueError, TypeError):
                    return False
            request = call.get('arguments', call.get('input', ''))
            output = payload.get('output')
            if dispatch is None and isinstance(request, str) and isinstance(output, str):
                for path, digest in hashes.items():
                    if path in request and hashlib.sha256(output.encode()).hexdigest() == digest:
                        reads[path] = output
    if not isinstance(spawn_output, dict) or not isinstance(spawn.get('agent_path'), str) or spawn_output.get('task_name') != spawn['agent_path']:
        return False
    if set(reads) != set(hashes) or not isinstance(dispatch, dict):
        return False
    role = next((text for path, text in reads.items() if path.endswith('/agents/worker.md')), None)
    if role is None or dispatch.get('fork_turns') != 'none' or dispatch.get('model') != 'gpt-6.1-terra' or dispatch.get('reasoning_effort') != 'medium':
        return False
    message = dispatch.get('message')
    if not isinstance(message, str) or role not in message:
        return False
    child_finished = any(e.get('type') == 'event_msg' and isinstance(e.get('payload'), dict) and e['payload'].get('type') == 'task_complete' for e in child_events if isinstance(e, dict))
    child_used_fixture = any(e.get('type') == 'response_item' and isinstance(e.get('payload'), dict) and '/workspace/RESULT.txt' in str(e['payload'].get('arguments', e['payload'].get('input', ''))) for e in child_events if isinstance(e, dict))
    return child_finished and child_used_fixture
