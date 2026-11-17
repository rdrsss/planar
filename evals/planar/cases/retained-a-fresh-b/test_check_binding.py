import json
from datetime import datetime, timezone
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from check_binding import verify, HOME
from check_evidence import CHECK, WORKDIR
from native import MissingEvidence, InvalidBehavior
from packets import TASK

BASE=int(datetime(2026,1,1,tzinfo=timezone.utc).timestamp()*1_000_000_000)
def timestamp(seconds): return datetime.fromtimestamp((BASE+int(seconds*1e9))/1e9,timezone.utc).isoformat()
def ev(kind,key,seconds,**data): return {'timestamp':timestamp(seconds),'type':'response_item','payload':{'type':kind,'call_id':key,**data}}
def call(key,cmd,seconds): return ev('function_call',key,seconds,name='exec_command',arguments=json.dumps({'cmd':cmd,'workdir':WORKDIR}))
def output(key,text,seconds,exit_code=0): return ev('function_call_output',key,seconds,output=json.dumps({'output':text,'exit_code':exit_code}))
def spawn(key,model,seconds): return ev('function_call',key,seconds,namespace='collaboration',name='spawn_agent',arguments=json.dumps({'model':model}))
def receipt(seconds): return {'wall_ns':BASE+int(seconds*1e9),'monotonic_ns':BASE+int(seconds*1e9)}
def build():
    header='---\ntask: T-001\n---\n\n## Log\n'
    row1='- 2026-01-01 00:00 | started | attempt 1 | session unavailable/unavailable | skill | I-003: session-I-003-1.md'
    row2='- 2026-01-01 00:01 | started | attempt 2 | session unavailable/unavailable | skill | I-007: session-I-007-1.md'
    log1=row1+'\n'; log2=log1+'- 2026-01-01 00:01 | checks-done | attempt 1 | session unavailable/unavailable | skill | checks-1.md\n'+row2+'\n'
    obs1='Task: T-001; attempt: 1; step: checks-done; role: session driver; invocation: I-003; phase: started\n'
    obs2='Task: T-001; attempt: 2; step: checks-done; role: session driver; invocation: I-007; phase: started\n'
    final={TASK+'record.md':(header+log2).encode(),TASK+'session-I-003-1.md':obs1.encode(),TASK+'session-I-007-1.md':obs2.encode()}
    for name in ('reviewer-packet-1.md','worker-packet-2.md','reviewer-packet-2.md'): final[TASK+name]=(name+' full\n').encode()
    snaps={'check-start-1':{TASK+'record.md':(header+log1).encode(),TASK+'session-I-003-1.md':obs1.encode()},
           'check-start-2':{TASK+'record.md':(header+log2).encode(),TASK+'session-I-003-1.md':obs1.encode(),TASK+'session-I-007-1.md':obs2.encode()},
           'packet-review-1':{TASK+'reviewer-packet-1.md':final[TASK+'reviewer-packet-1.md']},
           'packet-worker-2':{TASK+'worker-packet-2.md':final[TASK+'worker-packet-2.md']},
           'packet-review-2':{TASK+'reviewer-packet-2.md':final[TASK+'reviewer-packet-2.md']}}
    times={'check-start-1':.5,'packet-review-1':2.5,'packet-worker-2':3.5,'check-start-2':4.5,'packet-review-2':6.5}
    parent=[ev('function_call','dummy',0,name='other'),call('check1',CHECK,1),output('check1','',2),spawn('review1','gpt-6.1-sol',3),spawn('worker','gpt-6.1-terra',4),call('check2',CHECK,5),output('check2','',6),spawn('review2','gpt-6.1-sol',7)]
    children=[]
    for i,(name,seconds) in enumerate((('reviewer-packet-1.md',3.3),('worker-packet-2.md',4.3),('reviewer-packet-2.md',7.3))):
        text=final[TASK+name].decode(); children.append([call(f'child{i}',f'cat {HOME}{name}',seconds-.2),output(f'child{i}',text,seconds-.1),{'type':'event_msg','timestamp':timestamp(seconds),'payload':{'type':'task_complete'}}])
    return parent,final,children,snaps,times

class BindingControls(unittest.TestCase):
    def setUp(self):
        self.parent,self.final,self.children,self.snaps,self.times=build(); self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.target=Path(self.tmp.name)
        a=patch('capture.load',side_effect=lambda p:self.snaps[p.name]); a.start(); self.addCleanup(a.stop)
        b=patch('chronology.receipt',side_effect=lambda root,name:receipt(self.times[name])); b.start(); self.addCleanup(b.stop)
    def run_verify(self): return verify(self.parent,self.final,self.children,self.target,self.target,receipt(.1))
    def test_host_starts_and_child_packet_reads(self): self.assertEqual(self.run_verify()['call_ids'],['check1','check2'])
    def test_canonical_invocation_ids_need_no_role_keyword(self): self.assertEqual(len(self.run_verify()['child_packet_reads']),3)
    def test_parent_read_or_summary_is_not_child_delivery(self):
        self.children[0]=[{'type':'response_item','payload':{'type':'message','role':'user','content':'summary only'}}]
        with self.assertRaises(MissingEvidence): self.run_verify()
    def test_packet_capture_or_child_output_mismatch(self):
        self.snaps['packet-review-1'][TASK+'reviewer-packet-1.md']=b'summary'
        with self.assertRaises(InvalidBehavior): self.run_verify()
        self.snaps['packet-review-1'][TASK+'reviewer-packet-1.md']=self.final[TASK+'reviewer-packet-1.md']
        self.children[0][1]['payload']['output']=json.dumps({'output':'summary','exit_code':0})
        with self.assertRaises(MissingEvidence): self.run_verify()
    def test_check_start_must_be_host_captured_before_execution(self):
        self.times['check-start-1']=1.5
        with self.assertRaises(MissingEvidence): self.run_verify()
        self.times['check-start-1']=.5; self.times['check-start-2']=4.1
        with self.assertRaises(InvalidBehavior): self.run_verify()
    def test_conflicting_start_identity_rejected(self):
        key=TASK+'session-I-003-1.md'; self.snaps['check-start-1'][key]=self.snaps['check-start-1'][key].replace(b'invocation: I-003',b'invocation: I-004')
        with self.assertRaises(InvalidBehavior): self.run_verify()
    def test_failed_real_check_rejected(self):
        self.parent[2]['payload']['output']=json.dumps({'output':'','exit_code':1})
        with self.assertRaises(InvalidBehavior): self.run_verify()
    def test_wrong_reviewer_model_rejected(self):
        self.parent[3]['payload']['arguments']=json.dumps({'model':'gpt-6.1-terra'})
        with self.assertRaises(InvalidBehavior): self.run_verify()
    def test_pre_dispatch_capture_must_follow_work(self):
        self.times['packet-worker-2']=3.1
        with self.assertRaises(InvalidBehavior): self.run_verify()
        self.times['packet-worker-2']=3.5; self.times['packet-review-2']=5.5
        with self.assertRaises(MissingEvidence): self.run_verify()

if __name__=='__main__': unittest.main()
