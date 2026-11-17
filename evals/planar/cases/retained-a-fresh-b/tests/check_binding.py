"""Bind host-captured start/packet snapshots and native child delivery."""
import json
import re

from check_evidence import verify_two
from native import MissingEvidence, InvalidBehavior
from packets import TASK, _text
from review_structure import _rows

HOME='/workspace/home/projects/revision-fault/tasks/T-001/'
_LINK=re.compile(r'(?:\binvocation\s+([A-Za-z0-9_-]+)\s*;\s*|\b([A-Za-z0-9_-]+):\s*)(session-([A-Za-z0-9_-]+)-1\.md)')


def _observation(text):
    fields={}
    for line in text.splitlines():
        for part in line.lstrip('- ').split(';'):
            match=re.fullmatch(r'\s*([A-Za-z][A-Za-z ]*):\s*(.*?)\s*',part)
            if not match: continue
            key=match.group(1).lower()
            if key=='invocation id': key='invocation'
            if key in ('task','attempt','step','role','invocation','phase'):
                if key in fields: raise InvalidBehavior('duplicate check observation field')
                fields[key]=match.group(2)
    return fields


def _start_observation(final, attempt, rows):
    found=[]
    for row in rows:
        if row[1]!='started' or row[2]!=f'attempt {attempt}': continue
        matches=list(_LINK.finditer(row[5]))
        if len(matches)!=1: continue
        match=matches[0]; identity=match.group(1) or match.group(2)
        if identity != match.group(4): raise InvalidBehavior('check start link identity mismatch')
        name=match.group(3)
        obs=_text(final,TASK+name); fields=_observation(obs)
        if fields.get('step','').lower() not in ('checks-done','check','run checks'):
            continue
        expected={'task':'T-001','attempt':str(attempt),'invocation':identity}
        for key,value in expected.items():
            if key not in fields: raise MissingEvidence('check observation identity missing')
            if fields[key]!=value: raise InvalidBehavior('check observation identity mismatch')
        if fields.get('role','').lower() not in ('session','driver','session driver','check-runner'):
            raise MissingEvidence('unsupported check observation role')
        if fields.get('phase','').split(',',1)[0].strip().lower() not in ('start','started'):
            raise MissingEvidence('unsupported check start phase')
        found.append((name,obs,' | '.join(row)))
    if len(found)!=1: raise MissingEvidence('check start missing or ambiguous')
    return found[0]


def _event_calls(parent):
    calls={}; outputs={}; reviewers=[]
    for i,event in enumerate(parent):
        if not isinstance(event,dict) or event.get('type')!='response_item': continue
        p=event.get('payload')
        if not isinstance(p,dict): continue
        typ=p.get('type'); key=p.get('call_id')
        if typ in ('function_call','custom_tool_call'):
            if not isinstance(key,str) or key in calls: raise MissingEvidence('ambiguous native call')
            calls[key]=(i,p)
            if typ=='function_call' and p.get('namespace')=='collaboration' and p.get('name')=='spawn_agent':
                try: args=json.loads(p.get('arguments',''))
                except (TypeError,ValueError): raise MissingEvidence('invalid spawn arguments') from None
                reviewers.append((i,args))
        elif typ in ('function_call_output','custom_tool_call_output'):
            if not isinstance(key,str) or key in outputs: raise MissingEvidence('ambiguous native output')
            outputs[key]=(i,p)
    if len(reviewers)!=3: raise MissingEvidence('target spawn count unsupported')
    roles=[args.get('model') for _,args in reviewers if isinstance(args,dict)]
    if roles!=['gpt-6.1-sol','gpt-6.1-terra','gpt-6.1-sol']:
        raise InvalidBehavior('target role dispatch order mismatch')
    return calls,outputs,[item[0] for item in reviewers]


def _native_ns(event):
    from native import _time
    return int(_time(event).timestamp()*1_000_000_000)


def _stage(target, name):
    from capture import load
    from chronology import receipt
    return load(target/name), receipt(target,name)


def verify(parent, final, children, target, candidate, post_time):
    """Bind host snapshots, raw check results and the linked children's reads.

    Host integration must create the five snapshots/receipts while its target
    is paused, and must protect this evidence from the agent. A file authored
    by the agent or an unauthenticated timestamp cannot substitute. This helper
    does not prove the host envelope or issue a reward.
    """
    from pathlib import Path
    from chronology import order
    from native import _completion
    from delivery import verify_child_read
    target=Path(target)
    if not isinstance(children,list) or len(children)!=3: raise MissingEvidence('target child traces missing')
    record=_text(final,TASK+'record.md'); final_log=record.split('\n## Log\n',1)[1]
    calls,outputs,spawns=_event_calls(parent)
    checked=verify_two(parent,[(0,spawns[0]),(spawns[1],spawns[2])])
    check_calls=[calls[key][0] for key in checked['call_ids']]
    check_outputs=[outputs[key][0] for key in checked['call_ids']]
    for attempt in (1,2):
        snap,stamp=_stage(target,f'check-start-{attempt}')
        order(post_time,stamp)
        snap_record=_text(snap,TASK+'record.md')
        snap_log=snap_record.split('\n## Log\n',1)[1] if '\n## Log\n' in snap_record else ''
        if not snap_log or not final_log.startswith(snap_log): raise InvalidBehavior('check-start log prefix changed')
        name,obs,_row=_start_observation(snap,attempt,_rows(snap_record))
        if _text(final,TASK+name)!=obs: raise InvalidBehavior('check-start observation changed')
        if stamp['wall_ns'] >= _native_ns(parent[check_calls[attempt-1]]):
            raise MissingEvidence('check-start capture not before check')
        if attempt==2 and stamp['wall_ns'] <= _native_ns(_completion(children[1])):
            raise InvalidBehavior('second check start predates repair completion')
    packet_specs=[('packet-review-1','reviewer-packet-1.md',0),('packet-worker-2','worker-packet-2.md',1),('packet-review-2','reviewer-packet-2.md',2)]
    deliveries=[]
    for stage,filename,index in packet_specs:
        snap,stamp=_stage(target,stage); order(post_time,stamp)
        packet=_text(final,TASK+filename)
        if _text(snap,TASK+filename)!=packet: raise InvalidBehavior('pre-dispatch packet changed')
        if stamp['wall_ns'] >= _native_ns(parent[spawns[index]]):
            raise MissingEvidence('packet capture not before dispatch')
        if index==0 and stamp['wall_ns'] <= _native_ns(parent[check_outputs[0]]):
            raise MissingEvidence('first packet capture predates check completion')
        if index==1 and stamp['wall_ns'] <= _native_ns(_completion(children[0])):
            raise InvalidBehavior('repair packet predates first review completion')
        if index==2 and stamp['wall_ns'] <= _native_ns(parent[check_outputs[1]]):
            raise MissingEvidence('second packet capture predates check completion')
        deliveries.append(verify_child_read(children[index],filename,packet,candidate))
    if _native_ns(parent[check_calls[1]]) < _native_ns(_completion(children[1])):
        raise InvalidBehavior('second check predates repair completion')
    return {**checked,'child_packet_reads':deliveries}
