"""Bind cooperative record observations to native checks and child delivery.

Final records are not host attestations of intermediate writes. An independent
raw trace audit must establish check-start and packet-write timing.
"""
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


def verify(parent, final, children, target, candidate, post_time):
    """Check cooperative record structure, native checks, and child reads.

    No intermediate host capture is asserted. Final file order and observations
    are agent-authored; an independent bound raw audit remains mandatory before
    acceptance and must establish the relevant pre-check/pre-dispatch writes.
    """
    from native import _completion
    from delivery import verify_child_read
    if not isinstance(children,list) or len(children)!=3: raise MissingEvidence('target child traces missing')
    record=_text(final,TASK+'record.md'); rows=_rows(record)
    calls,outputs,spawns=_event_calls(parent)
    checked=verify_two(parent,[(0,spawns[0]),(spawns[1],spawns[2])])
    check_calls=[calls[key][0] for key in checked['call_ids']]
    for attempt in (1,2):
        _name,_obs,start_row=_start_observation(final,attempt,rows)
        start_indices=[i for i,row in enumerate(rows) if ' | '.join(row)==start_row]
        done_indices=[i for i,row in enumerate(rows) if row[1]=='checks-done' and row[2]==f'attempt {attempt}']
        if len(start_indices)!=1 or len(done_indices)!=1: raise MissingEvidence('check record ordering ambiguous')
        if start_indices[0] >= done_indices[0]: raise InvalidBehavior('check start logged after completion')
    packet_specs=[('reviewer-packet-1.md',0),('worker-packet-2.md',1),('reviewer-packet-2.md',2)]
    deliveries=[]
    for filename,index in packet_specs:
        packet=_text(final,TASK+filename)
        deliveries.append(verify_child_read(children[index],filename,packet,candidate))
    if _native_ns(parent[check_calls[1]]) <= _native_ns(_completion(children[1])):
        raise InvalidBehavior('second check predates repair completion')
    return {**checked,'child_packet_reads':deliveries,
            'check_starts':'cooperative-final-record',
            'pre_dispatch_writes':'independent-raw-audit-required'}
