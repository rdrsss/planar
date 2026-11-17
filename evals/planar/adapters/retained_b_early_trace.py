"""Exact, audited retained-B early-stop trace reader; no execution or release."""
import hashlib
import json

TRACE_SHA256 = '7ebac5fa973365f637365c54fdb675852623304767f45c6c9c895049a4b1e0cc'
THREAD = '01a1e823-ba08-7030-8683-9d5a06a817eb'
QUESTION = 'Can you confirm that the previous driver and all its children have stopped?'


def inspect_trace(raw):
    if not isinstance(raw,bytes) or hashlib.sha256(raw).hexdigest()!=TRACE_SHA256:
        raise ValueError('audited trace bytes differ')
    lines=raw.decode('utf-8').splitlines()
    if not lines or lines.pop(0)!='Reading additional input from stdin...': raise ValueError('unknown trace prologue')
    events=[json.loads(line) for line in lines]
    if sum(e.get('type')=='thread.started' for e in events)!=1 or events[0]!={'type':'thread.started','thread_id':THREAD}:
        raise ValueError('thread identity mismatch')
    if sum(e.get('type')=='turn.started' for e in events)!=1 or events[-1].get('type')!='turn.completed':
        raise ValueError('incomplete or multiple turns')
    starts={}; completions={}; terminal=None
    for n,event in enumerate(events):
        kind=event.get('type')
        if kind in ('thread.started','turn.started','turn.completed'): continue
        if kind not in ('item.started','item.completed'): raise ValueError('unsupported native event')
        item=event.get('item',{}); name=item.get('id'); typ=item.get('type')
        if name in ('item_0','item_1'):
            if kind!='item.completed' or typ!='error': raise ValueError('unexpected prelude')
        elif name=='item_2':
            if kind!='item.completed' or typ!='agent_message': raise ValueError('unexpected initial message')
        elif name=='item_13':
            if kind!='item.completed' or typ!='agent_message' or terminal is not None or QUESTION not in item.get('text',''):
                raise ValueError('handoff question mismatch')
            terminal=n
        elif name in {f'item_{i}' for i in range(3,13)} and typ=='command_execution':
            store=starts if kind=='item.started' else completions
            if name in store: raise ValueError('duplicate command event')
            store[name]=(n,item)
        else: raise ValueError('unexpected work or child event')
    expected={f'item_{i}' for i in range(3,13)}
    if set(starts)!=expected or set(completions)!=expected or terminal is None:
        raise ValueError('command evidence incomplete')
    for name in expected:
        a,first=starts[name]; b,last=completions[name]
        if a>=b or b>=terminal or first.get('command')!=last.get('command') or last.get('status')!='completed' or last.get('exit_code')!=0:
            raise ValueError('command chronology inconsistent')
    return {'thread_id':THREAD,'commands':10,'target_children':0,'question':QUESTION,'trace_sha256':TRACE_SHA256}
