"""Evidence that the linked child read the exact durable packet.

Recognizes direct native exec_command and the bounded observed single-call
Codex wrapper. It never evaluates JavaScript or shell source. Only bounded read-only `cat`, `sed`, or `head` forms for the selected packet
and installed candidate reference files are recognized.
"""
from pathlib import Path
import re
import shlex

from check_evidence import _args, _output, _paired, WORKDIR
from native import MissingEvidence, InvalidBehavior

HOME='/workspace/home/projects/revision-fault/tasks/T-001/'
PLUGIN='/installed-agent/codex-home/plugins/cache/planar-smoke-market/planar/0.1.0/'
_SAFE=re.compile(r'^/[A-Za-z0-9_./-]+$')


def _cat_content(command, filename, packet, candidate):
    try: parts=shlex.split(command,posix=True)
    except ValueError: return None
    if len(parts)<2: return None
    if parts[0]=='cat': paths=parts[1:]
    elif len(parts)==4 and parts[0]=='sed' and parts[1]=='-n' and re.fullmatch(r'1,[1-9][0-9]{0,4}p',parts[2]): paths=parts[3:]
    elif len(parts)==4 and parts[0]=='head' and parts[1] in ('-c','-n') and re.fullmatch(r'[1-9][0-9]{0,6}',parts[2]): paths=parts[3:]
    else: return None
    if any(not _SAFE.fullmatch(p) or '..' in Path(p).parts or '//' in p for p in paths): return None
    target=HOME+filename; found=0; pieces=[]
    for path in paths:
        if path==target:
            found+=1; pieces.append(packet)
        elif path.startswith(PLUGIN+'references/'):
            relative=path[len(PLUGIN):]; file=Path(candidate)/relative
            if file.is_symlink() or not file.is_file() or any(p.is_symlink() for p in list(file.parents)[:2]): return None
            try: pieces.append(file.read_text())
            except (UnicodeError,OSError): return None
        else: return None
    if found>1: raise InvalidBehavior('ambiguous packet read')
    return ''.join(pieces), bool(found)


def _contents(payload):
    parsed=_output(payload)
    if parsed is not None:
        if parsed.get('exit_code')!=0 or not isinstance(parsed.get('output'),str): return []
        return [parsed['output']]
    raw=payload.get('output')
    if isinstance(raw,str): return [raw]
    if isinstance(raw,list): return [part['text'] for part in raw if isinstance(part,dict) and isinstance(part.get('text'),str)]
    return []


def verify_child_read(child, filename, packet, candidate):
    """Return only after exact packet bytes appear in its own native read output."""
    if not isinstance(packet,str) or not packet: raise MissingEvidence('packet missing')
    calls={}; outputs={}
    for i,event in enumerate(child):
        if not isinstance(event,dict) or event.get('type')!='response_item': continue
        p=event.get('payload')
        if not isinstance(p,dict): continue
        typ=p.get('type'); key=p.get('call_id')
        if typ in ('function_call','custom_tool_call'):
            if not isinstance(key,str) or key in calls: raise MissingEvidence('ambiguous child call')
            calls[key]=(i,p)
        elif typ in ('function_call_output','custom_tool_call_output'):
            if not isinstance(key,str) or key in outputs: raise MissingEvidence('ambiguous child output')
            outputs[key]=(i,p)
    for key,(index,call) in sorted(calls.items(),key=lambda item:item[1][0]):
        args=_args(call, absolute_read=True)
        if not args or args['workdir'] not in (None, WORKDIR): raise MissingEvidence('unsupported pre-packet child tool call')
        expected=_cat_content(args['cmd'],filename,packet,candidate)
        if expected is None: raise MissingEvidence('unsupported pre-packet child read')
        if key not in outputs: raise MissingEvidence('child read output missing')
        oi,out=outputs[key]
        if oi<=index or not _paired(call,out) or expected[0] not in _contents(out):
            raise MissingEvidence('child read output not bound')
        if expected[1]:
            from native import _completion, _time
            done=_completion(child)
            if oi >= child.index(done) or _time(child[oi]) > _time(done):
                raise InvalidBehavior('child packet read after completion')
            return {'call_id':key,'call_index':index,'output_index':oi}
    raise MissingEvidence('child packet read missing')
