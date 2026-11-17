"""Narrow native call/output proof for the fixture's independent check.

The caller must independently bind start and reviewer event indices to the
record/observations and native reviewer dispatch. Unrecognized wrappers are
incomplete. This helper does not issue a reward or evaluate review packets.
"""
import json
import re

from native import MissingEvidence, InvalidBehavior

CHECK = r'''test -f RESULT.txt && test "$(od -An -tx1 RESULT.txt | tr -d ' \n')" = 320a'''
WORKDIR = '/workspace/repository'
_WRAPPER = re.compile(r'^\s*(?:const|let)\s+([A-Za-z_$][\w$]*)\s*=\s*await\s+tools\.exec_command\(\s*\{(.*?)\}\s*\)\s*;\s*text\s*\(\s*(?:JSON\.stringify\(\s*\1\s*\)|(\1)\.output)\s*\)\s*;?\s*$',re.S)
_READ_WRAPPER = re.compile(r'^\s*text\s*\(\s*await\s+tools\.exec_command\(\s*\{(.*?)\}\s*\)\s*\)\s*;?\s*$',re.S)
_KEY = re.compile(r'\s*(?:"([A-Za-z_]+)"|([A-Za-z_]+))\s*:\s*')
_ALLOWED={'cmd','workdir','max_output_tokens','yield_time_ms','tty'}


def _object(body):
    """Parse only a flat JS object of JSON scalar values; never evaluate code."""
    result={}; index=0; decoder=json.JSONDecoder()
    while index < len(body):
        match=_KEY.match(body,index)
        if not match: return None
        key=match.group(1) or match.group(2)
        if key not in _ALLOWED or key in result: return None
        try: value,index=decoder.raw_decode(body,match.end())
        except ValueError: return None
        if not isinstance(value,(str,int,bool)) or value is None: return None
        result[key]=value
        while index < len(body) and body[index].isspace(): index+=1
        if index==len(body): break
        if body[index]!=',': return None
        index+=1
        if not body[index:].strip(): return None
    return result


def _args(payload, *, absolute_read=False):
    value=None
    if payload.get('type') == 'function_call' and payload.get('name') == 'exec_command':
        try: value=json.loads(payload.get('arguments',''))
        except (ValueError,TypeError): return None
    elif payload.get('type') == 'custom_tool_call' and payload.get('name') == 'exec':
        raw=payload.get('input')
        if not isinstance(raw,str): return None
        match=_WRAPPER.fullmatch(raw)
        if match: value=_object(match.group(2))
        elif absolute_read:
            match=_READ_WRAPPER.fullmatch(raw)
            if match: value=_object(match.group(1))
    if not isinstance(value,dict) or not set(value)<=_ALLOWED or 'cmd' not in value or (not absolute_read and 'workdir' not in value): return None
    if not isinstance(value['cmd'],str) or ('workdir' in value and not isinstance(value['workdir'],str)): return None
    return {'cmd':value['cmd'],'workdir':value.get('workdir')}


def _output(payload):
    raw=payload.get('output')
    if isinstance(raw,list):
        raw=[part.get('text') for part in raw if isinstance(part,dict) and isinstance(part.get('text'),str)]
    elif isinstance(raw,str): raw=[raw]
    else: return None
    parsed=[]
    for item in raw:
        try: value=json.loads(item)
        except ValueError: continue
        if isinstance(value,dict) and 'exit_code' in value and 'output' in value: parsed.append(value)
    if len(parsed)!=1: return None
    return parsed[0]


def _paired(call, output):
    return (call.get('type'),output.get('type')) in (('function_call','function_call_output'),('custom_tool_call','custom_tool_call_output'))


def verify_two(parent, windows):
    """Verify two distinct check calls between caller-authenticated event indices.

    windows are (durable check-start event index, reviewer-spawn event index).
    The event stream must be the target parent, not an agent-authored log.
    """
    if len(windows)!=2: raise MissingEvidence('two check windows required')
    calls={}; outputs={}
    for index,event in enumerate(parent):
        if not isinstance(event,dict) or event.get('type')!='response_item': continue
        payload=event.get('payload')
        if not isinstance(payload,dict): continue
        kind=payload.get('type'); key=payload.get('call_id')
        if kind in ('function_call','custom_tool_call'):
            if not isinstance(key,str) or key in calls: raise MissingEvidence('ambiguous native call ID')
            calls[key]=(index,payload)
        elif kind in ('function_call_output','custom_tool_call_output'):
            if not isinstance(key,str) or key in outputs: raise MissingEvidence('ambiguous native output ID')
            outputs[key]=(index,payload)
    selected=[]
    for start,review in windows:
        if not (isinstance(start,int) and isinstance(review,int) and 0 <= start < review < len(parent)):
            raise MissingEvidence('invalid check window')
        found=[]
        for key,(index,payload) in calls.items():
            if not start < index < review: continue
            args=_args(payload)
            if args == {'cmd':CHECK,'workdir':WORKDIR}: found.append((key,index))
        if len(found)!=1: raise MissingEvidence('check call missing or ambiguous')
        key,index=found[0]
        if key not in outputs: raise MissingEvidence('check output missing')
        out_index,out=outputs[key]
        if not _paired(calls[key][1],out): raise MissingEvidence('native call/output type mismatch')
        if not index < out_index < review: raise InvalidBehavior('check output ordering')
        result=_output(out)
        if result is None or type(result.get('exit_code')) is not int or not isinstance(result.get('output'),str):
            raise MissingEvidence('check output unsupported')
        if result['exit_code'] != 0 or result['output'] != '': raise InvalidBehavior('check failed')
        selected.append(key)
    if len(set(selected))!=2: raise InvalidBehavior('check call reused')
    return {'checks':2,'call_ids':selected}
