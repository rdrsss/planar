"""Offline narrow dispatch assessment of a cached Harbor trial.

Host provenance and native rollouts are cross-checked; native logs are still
agent-writable, not adversarially authenticated. This never claims full role
instruction delivery, backend identity, or TEST-S69 acceptance.
"""
from __future__ import annotations

import hashlib
from datetime import datetime
import json
import re
import shlex
from pathlib import Path
import sys

from candidate import inventory
from check_provenance import check as check_provenance

ROOT = Path(__file__).resolve().parents[3]
CANDIDATE = ROOT / 'plugins/planar'
MANIFEST = ROOT / 'evals/planar/cases/plugin-smoke/candidate.json'
CACHE = '/installed-agent/codex-home/plugins/cache/planar-smoke-market/planar/0.1.0'
EXPECTED_IMAGE = 'sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7'
FILES = ('skills/run/SKILL.md', 'references/hosts.md', 'profiles/minimal.yaml', 'agents/worker.md')
SKILLS = ('check', 'draft', 'gap', 'project', 'replicate', 'review', 'run')
MAX_LOG = 4 * 1024 * 1024


def read_regular(path: Path, limit=MAX_LOG):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
        raise ValueError('missing, unsafe, or oversized evidence file')
    return path.read_bytes()


def events(path):
    raw = read_regular(path)
    result = []
    for line in raw.splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        if not isinstance(event, dict) or not isinstance(event.get('payload'), dict):
            raise ValueError('malformed event')
        result.append(event)
    if not result:
        raise ValueError('empty rollout')
    return result


def one_meta(ev):
    found = [e['payload'] for e in ev if e.get('type') == 'session_meta']
    if len(found) != 1 or not isinstance(found[0].get('id'), str):
        raise ValueError('missing or duplicate session metadata')
    return found[0]


def paired_calls(ev):
    calls, outputs = {}, {}
    for e in ev:
        if e.get('type') != 'response_item':
            continue
        p = e['payload']; kind = p.get('type'); key = p.get('call_id')
        if kind in ('function_call', 'custom_tool_call'):
            if not isinstance(key, str) or key in calls:
                raise ValueError('missing or duplicate call ID')
            calls[key] = p
        elif kind in ('function_call_output', 'custom_tool_call_output'):
            if not isinstance(key, str) or key in outputs:
                raise ValueError('missing or duplicate output ID')
            outputs[key] = p
    return calls, outputs


def output_text(payload):
    output = payload.get('output')
    if isinstance(output, str):
        return output
    if isinstance(output, list) and all(isinstance(x, dict) and isinstance(x.get('text'), str) for x in output):
        return ''.join(x['text'] for x in output)
    raise ValueError('malformed tool output')


def executable_call(call):
    """Recognize only the retained direct exec wrapper, never general JS."""
    raw = call.get('input')
    if not isinstance(raw, str):
        return None
    literal = r'"(?:\\.|[^"\\])*"'
    pattern = (r'\s*const\s+r\s*=\s*await\s+tools\.exec_command\(\{\s*cmd\s*:\s*('
               + literal + r')(?:\s*,\s*"workdir"\s*:\s*(' + literal
               + r'))?(?:\s*,\s*"max_output_tokens"\s*:\s*(\d+))?\s*\}\);\s*text\(r\.output\);\s*')
    match = re.fullmatch(pattern, raw)
    if match is None:
        return None
    try:
        command = json.loads(match.group(1))
        workdir = json.loads(match.group(2)) if match.group(2) else None
        maximum = int(match.group(3)) if match.group(3) else None
    except (ValueError, TypeError):
        return None
    if maximum is not None and not 0 < maximum <= 30000:
        return None
    return command, workdir


def shell_command(call):
    parsed = executable_call(call)
    return parsed[0] if parsed is not None else None


def event_time(event):
    raw = event.get('timestamp')
    if not isinstance(raw, str):
        raise ValueError('native event timestamp missing')
    try:
        value = datetime.fromisoformat(raw.replace('Z', '+00:00'))
    except ValueError:
        raise ValueError('invalid native event timestamp') from None
    if value.tzinfo is None:
        raise ValueError('native event timestamp lacks timezone')
    return value


def exec_workdir(call):
    parsed = executable_call(call)
    return parsed[1] if parsed is not None else None


def grade_rollouts(parent, child, expected_files):
    pmeta, cmeta = one_meta(parent), one_meta(child)
    parent_id, child_id = pmeta['id'], cmeta['id']
    if not parent_id or not child_id or parent_id == child_id or pmeta.get('source') != 'exec' or pmeta.get('originator') != 'codex_exec':
        raise ValueError('root or distinct child identity missing')
    source = cmeta.get('source', {})
    subagent = source.get('subagent') if isinstance(source, dict) else None
    spawn_meta = subagent.get('thread_spawn', {}) if isinstance(subagent, dict) else {}
    if not isinstance(spawn_meta, dict) or spawn_meta.get('parent_thread_id') != parent_id or cmeta.get('parent_thread_id') != parent_id or spawn_meta.get('depth') != 1:
        raise ValueError('child lineage mismatch')
    agent_path = spawn_meta.get('agent_path')
    if not isinstance(agent_path, str) or not agent_path.startswith('/root/') or cmeta.get('agent_path') != agent_path:
        raise ValueError('child agent path missing')
    calls, outputs = paired_calls(parent)
    spawn_calls = [c for c in calls.values() if c.get('type') == 'function_call' and c.get('name') == 'spawn_agent' and c.get('namespace') == 'collaboration']
    if len(spawn_calls) != 1:
        raise ValueError('expected exactly one native spawn')
    spawn = spawn_calls[0]
    args = json.loads(spawn.get('arguments', ''))
    if not isinstance(args, dict) or args.get('fork_turns') != 'none' or args.get('model') != 'gpt-6.1-terra' or args.get('reasoning_effort') != 'medium':
        raise ValueError('spawn freshness or requested pin mismatch')
    task_name = args.get('task_name')
    if not isinstance(task_name, str) or not re.fullmatch(r'[a-z0-9_]+', task_name) or agent_path != '/root/' + task_name:
        raise ValueError('requested task name does not match direct child path')
    result = json.loads(output_text(outputs.get(spawn['call_id'], {})))
    if not isinstance(result, dict) or result.get('task_name') != agent_path:
        raise ValueError('spawn result does not link child')
    spawn_event = next(event for event in parent if event.get('type') == 'response_item' and event['payload'] is spawn)
    output_event = next(event for event in parent if event.get('type') == 'response_item' and event['payload'] is outputs[spawn['call_id']])
    child_meta_event = next(event for event in child if event.get('type') == 'session_meta')
    if parent.index(output_event) <= parent.index(spawn_event) or not (event_time(spawn_event) <= event_time(child_meta_event) <= event_time(output_event)):
        raise ValueError('spawn and child creation order mismatch')
    reads = set()
    spawn_position = next(i for i, event in enumerate(parent) if event.get('type') == 'response_item' and event['payload'] is spawn)
    for key, call in calls.items():
        if key == spawn['call_id']:
            break
        if call.get('type') != 'custom_tool_call' or call.get('name') != 'exec' or key not in outputs:
            continue
        output_position = next(i for i, event in enumerate(parent) if event.get('type') == 'response_item' and event['payload'] is outputs[key])
        if output_position >= spawn_position:
            continue
        command, output = shell_command(call), output_text(outputs[key])
        if command is None or not output.startswith('Script completed\n'):
            continue
        try:
            tokens = shlex.split(command)
        except ValueError:
            continue
        allowed = {CACHE + '/' + name for name in expected_files}
        if not tokens or tokens[0] != 'cat' or not tokens[1:] or any(token not in allowed for token in tokens[1:]):
            continue
        for name, content in expected_files.items():
            if CACHE + '/' + name in tokens[1:] and content in output:
                reads.add(name)
    if reads != set(expected_files):
        raise ValueError('installed file read evidence incomplete')
    child_calls, child_outputs = paired_calls(child)
    completions = [(i, e) for i, e in enumerate(child) if e.get('type') == 'event_msg' and e['payload'].get('type') == 'task_complete']
    if len(completions) != 1:
        raise ValueError('child completion missing or ambiguous')
    complete_index, complete_event = completions[0]
    writes = []
    for key, call in child_calls.items():
        command = shell_command(call)
        if call.get('type') != 'custom_tool_call' or call.get('name') != 'exec' or exec_workdir(call) != '/workspace' or command is None or not re.fullmatch(r"printf\s+['\"]2\\n['\"]\s*>\s*(?:/workspace/)?RESULT\.txt", command) or key not in child_outputs:
            continue
        call_index = next(i for i,e in enumerate(child) if e.get('type') == 'response_item' and e['payload'] is call)
        output_index = next(i for i,e in enumerate(child) if e.get('type') == 'response_item' and e['payload'] is child_outputs[key])
        if call_index < output_index < complete_index and output_text(child_outputs[key]).startswith('Script completed\n') and event_time(child_meta_event) <= event_time(child[call_index]) <= event_time(child[output_index]) <= event_time(complete_event):
            writes.append(key)
    if len(writes) != 1:
        raise ValueError('ordered child result-write activity missing or ambiguous')
    def settings(ev):
        contexts = [e['payload'] for e in ev if e.get('type') == 'turn_context']
        if not contexts:
            raise ValueError('thread settings missing')
        return all(c.get('model') == 'gpt-6.1-terra' and c.get('effort') == 'medium' for c in contexts)
    if not settings(parent) or not settings(child):
        raise ValueError('thread settings mismatch')
    bodies = []
    for event in parent:
        if event.get('type') != 'world_state':
            continue
        state = event['payload'].get('state')
        skills = state.get('host_skills') if isinstance(state, dict) else None
        if isinstance(skills, dict):
            bodies.append(skills.get('body', ''))
    discovery = any(isinstance(body, str) and f'`{CACHE}/skills`' in body and all(f'- planar:{name}:' in body and f'(file: r0/{name}/SKILL.md)' in body for name in SKILLS) for body in bodies)
    return {'parent_id': parent_id, 'child_id': child_id, 'spawn_fresh_requested_pins': 'pass', 'distinct_child_lineage_and_completion': 'pass', 'installed_file_reads': 'pass', 'host_skill_inventory': 'pass' if discovery else 'pending', 'automatic_skill_discovery': 'unverified', 'thread_settings': 'pass', 'role_instruction_delivery': 'unverified', 'backend_identity': 'unverified'}


def regrade(evidence: Path):
    evidence = evidence.resolve(strict=True)
    check_provenance(evidence)
    start = json.loads(read_regular(evidence / 'launcher.json'))
    install = json.loads(read_regular(evidence / 'planar-install-provenance.json'))
    image = json.loads(read_regular(evidence / 'planar-container.json'))
    preauth = json.loads(read_regular(evidence / 'planar-preauth.json'))
    launch = json.loads(read_regular(evidence / 'planar-launch.json'))
    manifest = json.loads(read_regular(MANIFEST))
    if inventory(CANDIDATE) != manifest or start.get('candidate_sha256') != manifest['sha256'] or install.get('candidate_sha256') != manifest['sha256']:
        raise ValueError('frozen candidate digest mismatch')
    if image.get('image_id') != EXPECTED_IMAGE or preauth.get('image_id') != EXPECTED_IMAGE or preauth.get('status') != 'passed' or image.get('id') != preauth.get('container_id'):
        raise ValueError('container provenance mismatch')
    command = launch.get('command', '')
    if not isinstance(command, str) or '--model gpt-6.1-terra' not in command or 'model_reasoning_effort=medium' not in command:
        raise ValueError('native exec request mismatch')
    trials = list((evidence / evidence.name).glob('plugin-smoke__*/result.json'))
    if len(trials) != 1:
        raise ValueError('expected one cached trial')
    trial = trials[0].parent
    result = trial / 'artifacts/workspace/RESULT.txt'
    for node in (trial, trial/'artifacts', trial/'artifacts/workspace', trial/'agent', trial/'agent/sessions'):
        if node.is_symlink():
            raise ValueError('symlink in cached evidence path')
    if read_regular(result, 3) != b'2\n':
        raise ValueError('result bytes mismatch')
    paths = list((trial / 'agent/sessions').rglob('rollout-*.jsonl'))
    if len(paths) != 2 or any(any(node.is_symlink() for node in [path, *list(path.parents)[:4]]) for path in paths):
        raise ValueError('expected exactly two regular rollouts')
    rollouts = [events(path) for path in paths]
    parents = [e for e in rollouts if one_meta(e).get('source') == 'exec']
    if len(parents) != 1:
        raise ValueError('root rollout not unique')
    child = next(e for e in rollouts if e is not parents[0])
    expected = {name: (CANDIDATE / name).read_text() for name in FILES}
    dimensions = grade_rollouts(parents[0], child, expected)
    dimensions.update({'result_bytes': 'pass', 'frozen_installed_candidate_digest': 'pass', 'native_exec_host_provenance': 'pass'})
    return {'scope': 'cached-narrow-native-dispatch', 'status': 'pass', 'execution_id': evidence.name, 'dimensions': dimensions, 'limitations': ['native events are agent-writable', 'encoded role message prevents verifying complete instructions', 'host settings do not prove backend model or effort', 'host skill inventory does not independently prove automatic discovery', 'child tool output does not independently expose command exit status; result is a separate artifact', 'not TEST-S69 or full plugin acceptance']}


def main():
    try:
        print(json.dumps(regrade(Path(sys.argv[1])), indent=2, sort_keys=True))
    except (IndexError, OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
        print(json.dumps({'scope': 'cached-narrow-native-dispatch', 'status': 'inconclusive', 'reason': str(exc)}))
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
