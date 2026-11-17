import copy
import json
import unittest
from native import correlate, correlate_first, MissingEvidence, InvalidBehavior


def event(t, kind, payload):
    return {'timestamp': f'2026-01-01T00:00:{t:02d}Z', 'type': kind, 'payload': payload}


def examples():
    parents, children = [], []
    for pid, start, roles in [('a', 0, [('w1', 'gpt-6.1-terra')]), ('b', 15, [('r1', 'gpt-6.1-sol')])]:
        parent = [event(start, 'session_meta', {'id':pid, 'source':'exec', 'originator':'codex_exec'})]
        for i, (name, model) in enumerate(roles):
            t = start + 1 + i * 4
            args = {'fork_turns':'none', 'model':model, 'reasoning_effort':'medium', 'task_name':name}
            parent += [event(t, 'response_item', {'type':'function_call', 'namespace':'collaboration', 'name':'spawn_agent', 'call_id':name, 'arguments':json.dumps(args)}),
                       event(t+2, 'response_item', {'type':'function_call_output', 'call_id':name, 'output':json.dumps({'task_name':'/root/'+name})})]
            spawn = {'parent_thread_id':pid, 'agent_path':'/root/'+name, 'depth':1}
            children.append([event(t+1, 'session_meta', {'id':name, 'parent_thread_id':pid, 'agent_path':'/root/'+name, 'source':{'subagent':{'thread_spawn':spawn}}}), event(t+3, 'event_msg', {'type':'task_complete'})])
        parent.append(event(start+len(roles)*4+2, 'event_msg', {'type':'task_complete'}))
        parents.append(parent)
    return parents, children


def rebase(rows, start_ns, origin):
    from datetime import datetime, timezone
    result = copy.deepcopy(rows)
    for row in result:
        seconds = int(row['timestamp'][17:19]) - origin
        row['timestamp'] = datetime.fromtimestamp((start_ns+(seconds+2)*1000)/1_000_000_000, timezone.utc).isoformat()
    return result

class NativeTests(unittest.TestCase):
    def test_valid_and_fresh_identities(self):
        p, c = examples()
        self.assertEqual(correlate(p, c)['children'], ['w1', 'r1'])
    def test_first_boundary_has_exact_worker(self):
        p, c = examples()
        self.assertEqual(correlate_first(p[0], c[:1])['children'], ['w1'])
        with self.assertRaises(InvalidBehavior): correlate_first(p[0], c[:2])

    def test_missing_and_extra_rollouts(self):
        p, c = examples()
        with self.assertRaises(MissingEvidence): correlate(p, c[:-1])
        with self.assertRaises(InvalidBehavior): correlate(p, c + [copy.deepcopy(c[0])])
    def test_wrong_model_parent_and_overlap(self):
        p, c = examples()
        args = json.loads(p[1][1]['payload']['arguments']); args['model'] = 'gpt-6.1-terra'
        p[1][1]['payload']['arguments'] = json.dumps(args)
        with self.assertRaises(InvalidBehavior): correlate(p, c)
        p, c = examples(); c[1][0]['payload']['parent_thread_id'] = 'a'
        with self.assertRaises(InvalidBehavior): correlate(p, c)
        p, c = examples(); p[1][0]['timestamp'] = p[0][0]['timestamp']
        with self.assertRaises(InvalidBehavior): correlate(p, c)
    def test_missing_completion(self):
        p, c = examples(); c[1].pop()
        with self.assertRaises(MissingEvidence): correlate(p, c)

if __name__ == '__main__': unittest.main()
