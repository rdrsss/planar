"""Create explicit fake target events and host snapshots for noauth tests only.

This is not a Harbor run, target agent, real check, reviewer, or model outcome.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import time

from boundary import verify_injection
from capture import load, save
from test_review_structure import complete
from packets import TASK
from check_evidence import CHECK, WORKDIR
from delivery import HOME, PLUGIN


def tick():
    time.sleep(.002)
    return datetime.now(timezone.utc).isoformat()


def event(kind,payload): return {'timestamp':tick(),'type':kind,'payload':payload}
def call(key,cmd): return event('response_item',{'type':'function_call','name':'exec_command','call_id':key,'arguments':json.dumps({'cmd':cmd,'workdir':WORKDIR})})
def output(key,text): return event('response_item',{'type':'function_call_output','call_id':key,'output':json.dumps({'output':text,'exit_code':0})})


def spawn(parent,name,model,filename,packet,candidate):
    key='synth-spawn-'+name; args={'fork_turns':'none','model':model,'reasoning_effort':'medium','task_name':name}
    parent.append(event('response_item',{'type':'function_call','namespace':'collaboration','name':'spawn_agent','call_id':key,'arguments':json.dumps(args)}))
    path='/root/'+name; info={'parent_thread_id':'b','agent_path':path,'depth':1}
    child=[event('session_meta',{'id':name,'parent_thread_id':'b','agent_path':path,'source':{'subagent':{'thread_spawn':info}}})]
    parent.append(event('response_item',{'type':'function_call_output','call_id':key,'output':json.dumps({'task_name':path})}))
    ref=(candidate/'references/packets.md').read_text(); cmd=f'cat {PLUGIN}references/packets.md {HOME}{filename}'
    script='const r = await tools.exec_command({cmd:'+json.dumps(cmd)+',"workdir":"/workspace/repository","max_output_tokens":30000}); text(r.output)'
    ck='child-read-'+name
    child.append(event('response_item',{'type':'custom_tool_call','name':'exec','call_id':ck,'input':script}))
    child.append(event('response_item',{'type':'custom_tool_call_output','call_id':ck,'output':[{'text':'Script completed\n'},{'text':ref+packet}]}))
    child.append(event('event_msg',{'type':'task_complete'}))
    return child


def main(packet,candidate,destination):
    packet,candidate,destination=map(Path,(packet,candidate,destination))
    if destination.exists(): raise ValueError('synthetic target already exists')
    destination.mkdir(mode=0o700,parents=True)
    source=load(packet/'source/pre'); before=dict(source); save(destination/'restored-pre',before)
    started={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
    post=dict(before); post['repository/STATE.txt']=b'fault-injected\n'
    inv=json.loads(post['host/repository-inventory.json']); inv['files']['STATE.txt']={'size':len(post['repository/STATE.txt']),'sha256':hashlib.sha256(post['repository/STATE.txt']).hexdigest()}
    post['host/repository-inventory.json']=(json.dumps(inv,sort_keys=True,separators=(',',':'))+'\n').encode()
    ended={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}; save(destination/'post',post)
    change=verify_injection({k:v for k,v in before.items() if k!='host/repository-inventory.json'}, {k:v for k,v in post.items() if k!='host/repository-inventory.json'})
    (destination/'injection.json').write_text(json.dumps({'change':change,'started':started,'ended':ended}))
    _fixture, artificial=complete(); final=dict(source)
    for name,data in artificial.items():
        if name==TASK+'record.md' or (name.startswith(TASK) and name not in source): final[name]=data
    record=final[TASK+'record.md'].decode(); starts={}; observations={}
    for n,identity in ((1,'I-003'),(2,'I-007')):
        row=f'- 2026-11-17 03:20 | started | attempt {n} | session unavailable/unavailable | skill | {identity}: session-{identity}-1.md\n'
        needle=f'- 2026-11-17 03:20 | checks-done | attempt {n} |'
        if needle not in record: raise ValueError('synthetic transition missing')
        record=record.replace(needle,row+needle,1); starts[n]=row
        name=f'session-{identity}-1.md'; obs=f'Task: T-001; attempt: {n}; step: checks-done; role: session driver; invocation: {identity}; phase: started\n'
        observations[n]=(name,obs); final[TASK+name]=obs.encode()
    final[TASK+'record.md']=record.encode()
    def start_snapshot(n):
        row=starts[n]; end=record.index(row)+len(row); snap=dict(source); snap[TASK+'record.md']=record[:end].encode()
        for i in range(1,n+1):
            name,obs=observations[i]; snap[TASK+name]=obs.encode()
        save(destination/f'check-start-{n}',snap)
    def packet_snapshot(stage,name):
        snap=dict(source); snap[TASK+name]=final[TASK+name]; save(destination/stage,snap)
    parent=[event('session_meta',{'id':'b','source':'exec','originator':'codex_exec'})]; children=[]
    start_snapshot(1); parent += [call('synth-check-1',CHECK),output('synth-check-1','')]
    packet_snapshot('packet-review-1','reviewer-packet-1.md')
    children.append(spawn(parent,'r1','gpt-6.1-sol','reviewer-packet-1.md',final[TASK+'reviewer-packet-1.md'].decode(),candidate))
    packet_snapshot('packet-worker-2','worker-packet-2.md')
    children.append(spawn(parent,'w2','gpt-6.1-terra','worker-packet-2.md',final[TASK+'worker-packet-2.md'].decode(),candidate))
    start_snapshot(2); parent += [call('synth-check-2',CHECK),output('synth-check-2','')]
    packet_snapshot('packet-review-2','reviewer-packet-2.md')
    children.append(spawn(parent,'r2','gpt-6.1-sol','reviewer-packet-2.md',final[TASK+'reviewer-packet-2.md'].decode(),candidate))
    parent.append(event('event_msg',{'type':'task_complete'}))
    logs=destination/'logs/revision/sessions/synthetic'; logs.mkdir(parents=True)
    for i,rows in enumerate([parent,*children]): (logs/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    save(destination/'final',final)
    return {'mode':'synthetic-target-only','destination':str(destination),'real_target_execution':False}

if __name__=='__main__':
    if len(sys.argv)!=4: raise SystemExit('usage: synthetic_target.py PACKET CANDIDATE NEW_TARGET_DIRECTORY')
    print(json.dumps(main(*sys.argv[1:]),sort_keys=True))
