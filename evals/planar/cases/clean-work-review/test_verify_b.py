import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from datetime import datetime, timezone
from links import TASK, BASE
from no_fault import receipt as no_fault_receipt
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


class ReviewBoundaryTests(unittest.TestCase):
    def run_case(self, change=None):
        pre,post,dispatch,final,parents,children=evidence()
        if change: change(pre,post,dispatch,final,parents,children)
        stamp=int(datetime(2026,1,1,0,0,15,750000,tzinfo=timezone.utc).timestamp()*1e9)
        receipts={name:{'wall_ns':value,'monotonic_ns':i+1,'manifest_sha256':'a'*64} for i,(name,value) in enumerate((('pre',stamp-3),('post',stamp-2),('dispatch',stamp),('final',stamp+2_000_000_000)))}
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'no-fault.json').write_text(json.dumps(no_fault_receipt(pre,pre)))
            (root/'worker-gate.json').write_text(json.dumps({'reward':'1','pre_manifest_sha256':'a'*64}))
            with patch('verify_b.assess_first',return_value={'status':'pass'}), patch('verify_b.receipt',side_effect=lambda _,n:receipts[n]), patch('verify_b.load',side_effect=lambda p:{'pre':pre,'post':post,'dispatch':dispatch,'final':final}[p.name]), patch('verify_b.rollouts',side_effect=[(parents[0],children[0]),(parents[1],children[1])]), patch('verify_b.within_native'), patch('delivery.verify_child_read',return_value={'call_id':'child-read'}):
                result=assess(root)
                return 'structural' if result.get('structural_checks')=='passed' and result.get('status')=='incomplete' else result['status']
    def test_synthetic_review(self): self.assertEqual(self.run_case(), 'structural')
    def test_canonical_first_attempt_without_prior_review_with_markdown_checks(self):
        def canonical(a,b,c,d,p,k):
            packet=d[TASK+'reviewer-packet-1.md']
            packet=packet.replace(b'## Earlier reviews\n\nNone.\n\n',b'')
            packet=packet.replace(b'## Checks\n\n~~~\n',b'## Checks\n\n~~~~markdown\n')
            packet=packet.replace(b'status: complete\n~~~\n\nstatus: complete',b'status: complete\n~~~~\n\nstatus: complete')
            d[TASK+'reviewer-packet-1.md']=packet
        self.assertEqual(self.run_case(canonical),'structural')
    def test_changed_product_and_missing_packet(self):
        def product(a,b,c,d,p,k): b['repository/STATE.txt']=b'fault-injected\n'
        def packet(a,b,c,d,p,k): del d[TASK+'reviewer-packet-1.md']
        for change in (product,packet):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')
    def test_attempt_two_and_check_link_missing(self):
        def attempt(a,b,c,d,p,k): d[TASK+'record.md']+=b'- 2026-11-17 02:18 | started | attempt 2 | driver x/y | skill | x\n'
        def check(a,b,c,d,p,k): d[TASK+'record.md']=d[TASK+'record.md'].replace(b'session-C-1-1.md',b'not-linked')
        for change in (attempt,check):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')
    def test_changed_diff_and_packet_content(self):
        def diff(a,b,c,d,p,k): d['host/task-diff.txt']=d['host/task-diff.txt'].replace(b'+correct',b'+wrong')
        def cited(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'reviewer-packet-1.md']=x[TASK+'reviewer-packet-1.md'].replace(b'contains exactly `correct`',b'contains `correct`')
        for change in (diff,cited):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_missing_history_unattributed_check_and_late_capture(self):
        def history(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'reviewer-packet-1.md']=x[TASK+'reviewer-packet-1.md'].replace(b'## Earlier reviews\n\nNone.',b'## Earlier reviews\n\nInvented review.')
        def unattributed(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'reviewer-packet-1.md']=x[TASK+'reviewer-packet-1.md'].replace(b'## Unattributed changes\n\nNone.',b'## Unattributed changes\n\n')
        def late(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'session-R-1-1.md']=x[TASK+'session-R-1-1.md'].replace(b'2025-01-01T00:00:00+00:00',b'2027-01-01T00:00:00+00:00')
        for change in (history,unattributed,late):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_extra_reviewer_and_unattributed_path(self):
        def extra(a,b,c,d,p,k):
            p[1].insert(-1, {'timestamp':'2026-01-01T00:00:19Z','type':'response_item','payload':{'type':'function_call','namespace':'collaboration','name':'spawn_agent','call_id':'extra','arguments':'{}'}})
        def path(a,b,c,d,p,k): d['host/git-status.txt'] += b'?? OTHER.txt\n'
        for change in (extra,path):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_check_start_and_packet_timing(self):
        def late_check(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'session-C-1-1.md']=x[TASK+'session-C-1-1.md'].replace(b'2025-01-01T00:00:00+00:00',b'2027-01-01T00:00:00+00:00')
        def packet_drift(a,b,c,d,p,k): c[TASK+'reviewer-packet-1.md'] += b'late mutation'
        def premature_review(a,b,c,d,p,k): c[TASK+'review-1-all.md']=d[TASK+'review-1-all.md']
        for change in (late_check,):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_missing_cited_text_and_check_report(self):
        def citation(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'reviewer-packet-1.md']=x[TASK+'reviewer-packet-1.md'].replace(b'Create both root-level files exactly as PRD-R1 and PRD-R2 specify.',b'')
        def check_report(a,b,c,d,p,k):
            for x in (c,d): x[TASK+'reviewer-packet-1.md']=x[TASK+'reviewer-packet-1.md'].replace(b'Exit code: 0',b'')
        for change in (citation,check_report):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_check_native_result_must_match_and_succeed(self):
        def missing(a,b,c,d,p,k): p[1].pop(2)
        def nonzero(a,b,c,d,p,k): p[1][2]['payload']['output'][1]['text']=json.dumps({'exit_code':1,'output':'failure'})
        def wrong_id(a,b,c,d,p,k): p[1][2]['payload']['call_id']='unrelated-call'
        def duplicate(a,b,c,d,p,k): p[1].insert(3,copy.deepcopy(p[1][2]))
        def running(a,b,c,d,p,k): p[1][2]['payload']['output'][1]['text']=json.dumps({'session_id':7,'output':''})
        def wrong_shape(a,b,c,d,p,k): p[1][2]['payload']['output'][0]['text']='Script running\n'
        def output_after_spawn(a,b,c,d,p,k): p[1][2]['timestamp']='2026-01-01T00:00:17Z'
        for change in (missing,nonzero,wrong_id,duplicate,running,wrong_shape,output_after_spawn):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_check_must_be_actual_exact_shell_command(self):
        def command(a,b,c,d,p,k): p[1][1]['payload']['input']=p[1][1]['payload']['input'].replace(json.dumps(CHECK),json.dumps('echo '+CHECK))
        def unrelated(a,b,c,d,p,k): p[1][1]['payload']['namespace']='unrelated'
        def wrong_dir(a,b,c,d,p,k): p[1][1]['payload']['input']=p[1][1]['payload']['input'].replace('/workspace/repository','/tmp')
        def output_spoof(a,b,c,d,p,k): p[1][1]['payload']['input']=p[1][1]['payload']['input'].replace('text(JSON.stringify(r))',"text(0)")
        for change in (command,unrelated,wrong_dir,output_spoof):
            with self.subTest(change=change): self.assertNotEqual(self.run_case(change),'structural')

    def test_equivalent_native_wrapper_passes(self):
        def alternate(a,b,c,d,p,k):
            p[1][1]['payload']['input'] = ('let result = await tools.exec_command ( { "workdir" : "/workspace/repository", "cmd" : ' + json.dumps(CHECK) + ' } );\ntext ( result );')
        self.assertEqual(self.run_case(alternate), 'incomplete')

    def test_opaque_and_forged_wrappers_are_incomplete(self):
        def opaque(a,b,c,d,p,k): p[1][1]['payload']['input'] = 'await my_helper()'
        def forged(a,b,c,d,p,k): p[1][1]['payload']['input'] += '; text({exit_code: 0})'
        def wrong_var(a,b,c,d,p,k): p[1][1]['payload']['input'] = p[1][1]['payload']['input'].replace('JSON.stringify(r)', 'JSON.stringify(other)')
        for change in (opaque,forged,wrong_var):
            with self.subTest(change=change): self.assertEqual(self.run_case(change), 'incomplete')
