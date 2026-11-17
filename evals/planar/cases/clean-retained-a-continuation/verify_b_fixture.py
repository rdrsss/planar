import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from datetime import datetime, timezone
from links import TASK, BASE
from reviewer_packet import sections
from test_assess_worker import example
from test_native import examples
from verify_b import assess, CHECK


def evidence():
    pre=example()
    fixture=Path(__file__).parent/'fixture/home/projects/revision-fault'
    for name in ('prd.md','tech-spec.md','layout.md','testing-spec.md','tasks.md'):
        pre['home/projects/revision-fault/'+name]=(fixture/name).read_bytes()
    pre['host/git-head.txt']=(BASE+'\n').encode()
    pre['host/git-status.txt']=b'?? RESULT.txt\n?? STATE.txt\n'
    post=copy.deepcopy(pre); final=copy.deepcopy(pre)
    original=pre[TASK+'record.md'].decode()
    head=original.replace('state: worker-done','state: approved')
    def row(step, result, actor='session unavailable/unavailable'):
        return f'- 2026-11-17 02:17 | {step} | attempt 1 | {actor} | skill | {result}\n'
    check_obs=(f'Task: T-001; attempt: 1; step: checks-done; role: driver; invocation: C-1; parent: unavailable; phase: started; timestamp: 2025-01-01T00:00:00+00:00\nPacket: checks-1.md; result: checks-1.md; base: {BASE}\n').encode()
    final[TASK+'session-C-1-1.md']=check_obs
    checks=(f'# Checks: T-001, attempt 1\n\n## TEST-C1\n\nResult: passed\nCommand: `{CHECK}`\nExit code: 0\n\nstatus: complete\n')
    final[TASK+'checks-1.md']=checks.encode()
    worker=pre[TASK+'worker-packet-1.md'].decode(); shared=sections(worker.rsplit('\nstatus: complete',1)[0]+'\n')
    diff='diff --git a/RESULT.txt b/RESULT.txt\nnew file mode 100644\n--- /dev/null\n+++ b/RESULT.txt\n@@ -0,0 +1 @@\n+2\ndiff --git a/STATE.txt b/STATE.txt\nnew file mode 100644\n--- /dev/null\n+++ b/STATE.txt\n@@ -0,0 +1 @@\n+correct\n'
    packet='# Reviewer packet: T-001, attempt 1\n\n## Resolved configuration\n\nreviewer gpt-6.1-sol medium; revision-fault.yaml ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7; session owns child.\n\n'
    for key in ('Task','Paths you may create or change','Cited spec sections, in full','Checks this task must pass'):
        packet+=f'## {key}\n\n{shared[key]}\n\n'
    packet+=f'## Change\n\n~~~diff\n{diff}~~~\n\n## Unattributed changes\n\nNone.\n\n## Earlier uncommitted tasks\n\nNone.\n\n## Checks\n\n~~~\n{checks}~~~\n\n## Earlier reviews\n\nNone.\n\nstatus: complete\n'
    final[TASK+'reviewer-packet-1.md']=packet.encode()
    review='# Review: T-001, attempt 1, all\n\nverdict: approve\nstatus: complete\n'
    final[TASK+'review-1-all.md']=review.encode()
    def observation(n, phase):
        return (f'Task: T-001; attempt: 1; step: reviewed; role: reviewer; invocation: R-1; parent: I-001; phase: {phase}; timestamp: 2025-01-01T00:00:00+00:00\nPacket: reviewer-packet-1.md; result: review-1-all.md; base: {BASE}\nResource ownership: driver owns reviewer child.\n').encode()
    for n, phase in enumerate(('started','dispatch accepted','completed'),1): final[TASK+f'session-R-1-{n}.md']=observation(n,phase)
    prefix=(row('started','check observation session-C-1-1.md')+row('checks-done','checks-1.md')+
            row('review-packet-rendered','reviewer-packet-1.md')+row('started','review session-R-1-1.md', 'reviewer gpt-6.1-sol/medium'))
    suffix=(row('session-observed','session-R-1-2.md')+row('session-observed','session-R-1-3.md')+
            row('reviewed','review-1-all.md: approve')+row('approved','task approved'))
    final[TASK+'record.md']=(head+prefix+suffix).encode()
    final['home/projects/revision-fault/tasks.md']=pre['home/projects/revision-fault/tasks.md'].replace(b'- [ ] **T-001**',b'- [x] **T-001**')
    dispatch=copy.deepcopy(final); dispatch[TASK+'record.md']=(head.replace('state: approved','state: review-packet-rendered')+prefix).encode()
    del dispatch[TASK+'session-R-1-2.md']; del dispatch[TASK+'session-R-1-3.md']; del dispatch[TASK+'review-1-all.md']
    dispatch['host/task-diff.txt']=diff.encode(); final['host/task-diff.txt']=diff.encode()
    parents, children=examples()
    script=('const r = await tools.exec_command({cmd:' + json.dumps(CHECK) + ',workdir:"/workspace/repository"}); text(JSON.stringify(r));')
    command={'timestamp':'2026-01-01T00:00:15.500000Z','type':'response_item','payload':{'type':'custom_tool_call','name':'exec','call_id':'check-command','input':script}}
    result={'timestamp':'2026-01-01T00:00:15.600000Z','type':'response_item','payload':{'type':'custom_tool_call_output','call_id':'check-command','output':[{'type':'input_text','text':'Script completed\nWall time 0.1 seconds\nOutput:\n'},{'type':'input_text','text':json.dumps({'exit_code':0,'output':'','wall_time_seconds':0.1})}]}}
    parents[1].insert(1,command); parents[1].insert(2,result)
    return pre,post,dispatch,final,parents,children


