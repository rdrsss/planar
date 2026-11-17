"""Bounded native lineage and exported artifact assessment; no command parsing."""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
from datetime import datetime

BUNDLE = Path(__file__).resolve().parent / 'bundle'
FROZEN = '7258e3bb0c5d451554ea0a0a163ab16afabbbdd34013f37a7a19fbd9b208a140'
CACHE = '/installed-agent/codex-home/plugins/cache/planar-smoke-market/planar/0.1.0'
SKILLS = ('check', 'draft', 'gap', 'project', 'replicate', 'review', 'run')
IDENTITY = {'case':'planar/native-lineage-current', 'contract_version':1, 'scope':'recorded-native-lineage-and-artifact', 'full_test_s69':'pending'}
LIMITATIONS = ['native events are agent-writable', 'resource reads and their order are unverified', 'causal child write is unverified', 'role instruction delivery is unverified', 'automatic skill activation is unverified', 'backend identity is unverified', 'host launch, installed whole-package provenance and initial empty workspace are unverified by this verifier']

class EvidenceError(ValueError): pass
class BehaviorError(ValueError): pass

def outcome(status, reason=None, **fields):
    result = {**IDENTITY, 'status':status, 'limitations':LIMITATIONS, **fields}
    if reason: result['reason'] = reason
    return result

def regular(path, limit=4*1024*1024):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > limit: raise EvidenceError('missing or unsafe evidence')
    return path.read_bytes()

def manifest_check(bundle):
    manifest = json.loads(regular(bundle/'manifest.json'))
    entries = manifest['files']
    canonical = json.dumps(entries, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()
    if manifest.get('algorithm') != 'sha256-json-inventory-v1' or manifest.get('sha256') != FROZEN or hashlib.sha256(canonical).hexdigest() != FROZEN:
        raise EvidenceError('frozen candidate manifest mismatch')

def events(path):
    rows=[]
    for line in regular(path).splitlines():
        if not line.strip(): continue
        event=json.loads(line)
        if not isinstance(event,dict) or not isinstance(event.get('payload'),dict): raise EvidenceError('malformed native event')
        rows.append(event)
    if not rows: raise EvidenceError('empty native rollout')
    return rows

def metadata(rows):
    found=[e['payload'] for e in rows if e.get('type')=='session_meta']
    if len(found)!=1 or not isinstance(found[0].get('id'),str): raise EvidenceError('missing native metadata')
    return found[0]

def timestamp(event):
    try: value=datetime.fromisoformat(event['timestamp'].replace('Z','+00:00'))
    except (KeyError, ValueError, TypeError, AttributeError): raise EvidenceError('invalid native timestamp') from None
    if value.tzinfo is None: raise EvidenceError('native timestamp lacks timezone')
    return value

def grade(parent, child):
    pm, cm=metadata(parent),metadata(child); pid,cid=pm['id'],cm['id']
    if not pid or not cid or pid==cid or pm.get('source')!='exec' or pm.get('originator')!='codex_exec': raise BehaviorError('root or child identity mismatch')
    source=cm.get('source'); sub=source.get('subagent') if isinstance(source,dict) else None
    spawnmeta=sub.get('thread_spawn') if isinstance(sub,dict) else None
    if not isinstance(spawnmeta,dict): raise EvidenceError('child spawn metadata missing')
    path=spawnmeta.get('agent_path')
    if spawnmeta.get('parent_thread_id')!=pid or cm.get('parent_thread_id')!=pid or spawnmeta.get('depth')!=1 or not isinstance(path,str) or cm.get('agent_path')!=path: raise BehaviorError('child lineage mismatch')
    calls={};outputs={}
    for index,event in enumerate(parent):
        if event.get('type')!='response_item': continue
        p=event['payload'];kind=p.get('type');key=p.get('call_id')
        if kind not in ('function_call','function_call_output','custom_tool_call','custom_tool_call_output'): continue
        target=calls if kind in ('function_call','custom_tool_call') else outputs
        if not isinstance(key,str) or key in target: raise EvidenceError('missing or duplicate native call id')
        target[key]=(index,event)
    spawns=[(key,index,event) for key,(index,event) in calls.items() if event['payload'].get('type')=='function_call' and event['payload'].get('name')=='spawn_agent' and event['payload'].get('namespace')=='collaboration']
    if len(spawns)!=1: raise BehaviorError('expected one native spawn')
    key,index,event=spawns[0]
    try: args=json.loads(event['payload']['arguments'])
    except (KeyError,TypeError,json.JSONDecodeError): raise EvidenceError('malformed spawn arguments') from None
    if not isinstance(args,dict): raise EvidenceError('malformed spawn arguments')
    if args.get('fork_turns')!='none' or args.get('model')!='gpt-6.1-terra' or args.get('reasoning_effort')!='medium': raise BehaviorError('spawn freshness or requested pin mismatch')
    task=args.get('task_name')
    if not isinstance(task,str) or not re.fullmatch(r'[a-z0-9_]+',task) or path!='/root/'+task: raise BehaviorError('spawn task path mismatch')
    if key not in outputs: raise EvidenceError('spawn output missing')
    outidx,outevent=outputs[key]
    if outevent['payload'].get('type')!='function_call_output': raise EvidenceError('unexpected spawn output kind')
    raw=outevent['payload'].get('output')
    if isinstance(raw,list) and all(isinstance(x,dict) and isinstance(x.get('text'),str) for x in raw): raw=''.join(x['text'] for x in raw)
    try: result=json.loads(raw)
    except (TypeError,json.JSONDecodeError): raise EvidenceError('malformed spawn output') from None
    if not isinstance(result,dict) or not isinstance(result.get('task_name'),str): raise EvidenceError('unsupported spawn output')
    if result['task_name']!=path: raise BehaviorError('spawn output child mismatch')
    meta_event=next(e for e in child if e.get('type')=='session_meta')
    if outidx<=index or not(timestamp(event)<=timestamp(meta_event)<=timestamp(outevent)): raise BehaviorError('spawn creation order mismatch')
    parent_completions=[(i,e) for i,e in enumerate(parent) if e.get('type')=='event_msg' and e['payload'].get('type')=='task_complete']
    if len(parent_completions)!=1: raise EvidenceError('ambiguous parent completion')
    completion_index,completion_event=parent_completions[0]
    if completion_index<=outidx or timestamp(completion_event)<timestamp(outevent): raise BehaviorError('parent completion order mismatch')
    completions=[e for e in child if e.get('type')=='event_msg' and e['payload'].get('type')=='task_complete']
    if len(completions)!=1: raise EvidenceError('ambiguous child completion')
    if child.index(completions[0])<=child.index(meta_event) or timestamp(completions[0])<timestamp(meta_event): raise BehaviorError('child completion order mismatch')
    observations={};mismatch=False
    for label,rows in (('parent',parent),('child',child)):
        contexts=[e['payload'] for e in rows if e.get('type')=='turn_context'];observations[label]={}
        for field,expected in (('model','gpt-6.1-terra'),('effort','medium')):
            values=[c[field] for c in contexts if c.get(field) is not None]
            if any(not isinstance(v,str) for v in values): raise EvidenceError('malformed thread settings')
            mismatch |= any(v!=expected for v in values)
            observations[label][field]={'values':values,'source':'native turn_context' if values else 'unavailable'}
    if mismatch: return outcome('behavioral_failure','available thread settings contradict requested pins',configured_worker={'model':'gpt-6.1-terra','effort':'medium','source':'native spawn arguments'},observed_thread_settings=observations,disposition='operator_question_before_dispatch_or_approval')
    inventories=[]
    for index,event in enumerate(parent):
        if event.get('type')!='world_state': continue
        state=event['payload'].get('state')
        if isinstance(state,dict) and 'host_skills' in state: inventories.append((index,state['host_skills']))
    if len(inventories)>1: raise EvidenceError('ambiguous host skill inventory')
    if not inventories: raise BehaviorError('required host skill inventory missing')
    inv_index,skills=inventories[0]
    if not isinstance(skills,dict) or not isinstance(skills.get('body'),str): raise EvidenceError('malformed host skill inventory')
    actions=[i for i,e in enumerate(parent) if e.get('type')=='response_item' and (e['payload'].get('type') in ('reasoning','function_call','custom_tool_call','agent_message') or (e['payload'].get('type')=='message' and e['payload'].get('role')=='assistant'))]
    if not actions or inv_index>=min(actions): raise BehaviorError('host skill inventory is not initial')
    body=skills['body']
    if f'`{CACHE}/skills`' not in body or not all(f'- planar:{name}:' in body and f'(file: r0/{name}/SKILL.md)' in body for name in SKILLS): raise BehaviorError('required host skill inventory mismatch')
    return {'parent_id':pid,'child_id':cid,'spawn_fresh_requested_pins':'pass','direct_child_lineage':'pass','host_skill_inventory':'pass','thread_settings':observations,'resource_reads':'unverified','read_before_spawn':'unverified','child_write_causality':'unverified','role_instruction_delivery':'unverified','backend_identity':'unverified'}

def assess(workspace=Path('/workspace'),agent=Path('/logs/agent'),bundle=BUNDLE):
    manifest_check(bundle)
    if agent.is_symlink() or not agent.is_dir() or (agent/'sessions').is_symlink(): raise EvidenceError('missing or unsafe agent sessions')
    paths=list((agent/'sessions').rglob('rollout-*.jsonl'))
    if len(paths)!=2 or any(any(node.is_symlink() for node in [p,*list(p.parents)[:4]]) for p in paths): raise EvidenceError('expected exactly two safe rollouts')
    rollouts=[events(p) for p in paths];parents=[r for r in rollouts if metadata(r).get('source')=='exec']
    if len(parents)!=1: raise EvidenceError('root rollout not unique')
    child=next(r for r in rollouts if r is not parents[0])
    if any(not any(e.get('type')=='event_msg' and e['payload'].get('type')=='task_complete' for e in rows) for rows in (parents[0],child)): return outcome('incomplete','native completion evidence missing')
    if workspace.is_symlink() or not workspace.is_dir(): raise EvidenceError('missing or unsafe workspace export')
    try: dimensions=grade(parents[0],child)
    except BehaviorError as exc: return outcome('behavioral_failure',str(exc))
    if dimensions.get('status')=='behavioral_failure': return dimensions
    for item in workspace.iterdir():
        if item.name!='RESULT.txt' or item.is_symlink(): return outcome('behavioral_failure','unexpected or unsafe fixture entry')
    try:
        path=workspace/'RESULT.txt'
        if not stat.S_ISREG(path.lstat().st_mode): return outcome('behavioral_failure','missing or invalid result')
        fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    except OSError: return outcome('behavioral_failure','missing or invalid result')
    with os.fdopen(fd,'rb') as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode) or stream.read(3)!=b'2\n': return outcome('behavioral_failure','wrong result bytes')
    dimensions['result_bytes']='pass';dimensions['host_provenance']='unverified';dimensions['pre_execution_workspace']='unverified';dimensions['frozen_candidate_manifest']=FROZEN
    return outcome('pass',dimensions=dimensions,configured_worker={'model':'gpt-6.1-terra','effort':'medium','source':'native spawn arguments'})

def main():
    try: result=assess()
    except (OSError,ValueError,TypeError,KeyError,json.JSONDecodeError) as exc: result=outcome('infrastructure_error',str(exc))
    print(json.dumps(result,sort_keys=True))
    return {'pass':0,'behavioral_failure':1}.get(result['status'],2)
if __name__=='__main__': sys.exit(main())
