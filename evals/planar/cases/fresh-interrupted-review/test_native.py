import copy
import json
import unittest
from native import correlate, MissingEvidence, InvalidBehavior


def event(t, kind, payload): return {'timestamp':f'2026-01-01T00:00:{t:02d}Z', 'type':kind, 'payload':payload}
def examples():
    parent = [event(0,'session_meta',{'id':'p','source':'exec','originator':'codex_exec'}),
              event(1,'response_item',{'type':'function_call','namespace':'collaboration','name':'spawn_agent','call_id':'x','arguments':json.dumps({'fork_turns':'none','model':'gpt-6.1-sol','reasoning_effort':'medium','task_name':'reviewer'})}),
              event(3,'response_item',{'type':'function_call_output','call_id':'x','output':json.dumps({'task_name':'/root/reviewer'})}),
              event(6,'event_msg',{'type':'task_complete'})]
    child = [event(2,'session_meta',{'id':'c','parent_thread_id':'p','agent_path':'/root/reviewer','source':{'subagent':{'thread_spawn':{'parent_thread_id':'p','agent_path':'/root/reviewer','depth':1}}}}), event(5,'event_msg',{'type':'task_complete'})]
    return parent,child

class NativeControls(unittest.TestCase):
    def test_valid(self):
        p,c = examples(); self.assertEqual(correlate(p,c)['child_id'],'c')
    def test_missing_completion(self):
        p,c=examples(); c.pop()
        with self.assertRaises(MissingEvidence): correlate(p,c)
    def test_extra_dispatch(self):
        p,c=examples(); e=copy.deepcopy(p[1]); e['payload']['call_id']='y'; p.insert(2,e)
        with self.assertRaises(InvalidBehavior): correlate(p,c)
    def test_wrong_parent(self):
        p,c=examples(); c[0]['payload']['parent_thread_id']='other'
        with self.assertRaises(InvalidBehavior): correlate(p,c)
    def test_bad_arguments(self):
        p,c=examples(); p[1]['payload']['arguments']='{'
        with self.assertRaises(MissingEvidence): correlate(p,c)
