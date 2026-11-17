import unittest
from native import MissingEvidence, InvalidBehavior
from check_evidence import CHECK
from packets import TASK
from test_packets import source_fixture, make_packet
from test_assess_worker import retained_live
from review_structure import verify


def complete():
    source=source_fixture(); source.update(retained_live())
    source.update({'host/git-head.txt':b'base','host/git-stage.txt':b'stage','host/git-status.txt':b'status'})
    final=dict(source); record=source[TASK+'record.md'].decode().replace('state: worker-done','state: reviewed').replace('attempt: 1\nbase:','attempt: 2\nbase:')
    steps=[('checks-done',1,'checks-1.md'),('review-packet-rendered',1,'reviewer-packet-1.md'),('reviewed',1,'review-1-all.md'),('changes-requested',1,'review-1-all.md'),('packet-rendered',2,'worker-packet-2.md'),('worker-done',2,'worker-report-2.md'),('checks-done',2,'checks-2.md'),('review-packet-rendered',2,'reviewer-packet-2.md'),('reviewed',2,'review-2-all.md')]
    for step,n,detail in steps: record+=f'- 2026-11-17 03:20 | {step} | attempt {n} | session unavailable/unavailable | skill | {detail}\n'
    final[TASK+'record.md']=record.encode()
    finding='- **F1** cites: PRD-R2. STATE.txt is fault-injected; correct it.'
    prior=('# Review: T-001, attempt 1, all\n\n'+finding+'\n\nverdict: request-changes\nstatus: complete\n').encode()
    final[TASK+'review-1-all.md']=prior
    for n in (1,2):
        tmp=make_packet(source,n); key=TASK+f'reviewer-packet-{n}.md'; old=tmp[TASK+f'checks-{n}.md']
        check=(f'# Checks {n}\nCommand: `{CHECK}`\nWorking directory: /workspace/repository\nOutput: (empty)\nExit code: 0\nResult: passed\nstatus: complete\n').encode()
        packet=tmp[key].replace(old.strip(),check.strip())
        if n==2: packet=packet.replace(tmp[TASK+'review-1-all.md'].strip(),prior.strip())
        final[TASK+f'checks-{n}.md']=check; final[key]=packet
    final[TASK+'worker-packet-2.md']=prior+b'\nstatus: complete\n'
    final[TASK+'worker-report-2.md']=b'status: done\n'
    final[TASK+'review-2-all.md']=b'# Review: T-001, attempt 2, all\nverdict: approve\nstatus: complete\n'
    return source,final

class RevisionStructureControls(unittest.TestCase):
    def test_full_structure_needs_native_and_semantic_checks(self):
        source,final=complete(); self.assertEqual(verify(source,final)['native_check_proof'],'required')
    def test_source_rewrite_and_wrong_final_refused(self):
        source,final=complete(); final[TASK+'worker-report-1.md']=b'rewritten'
        with self.assertRaises(InvalidBehavior): verify(source,final)
        source,final=complete(); final['repository/STATE.txt']=b'fault-injected\n'
        with self.assertRaises(InvalidBehavior): verify(source,final)
    def test_finding_transfer_and_full_review_required(self):
        source,final=complete(); final[TASK+'worker-packet-2.md']=b'summary\nstatus: complete\n'
        with self.assertRaises(MissingEvidence): verify(source,final)
        source,final=complete(); final[TASK+'reviewer-packet-2.md']=final[TASK+'reviewer-packet-2.md'].replace(b'STATE.txt is fault-injected; correct it.',b'finding summary')
        with self.assertRaises(MissingEvidence): verify(source,final)
    def test_check_report_cannot_be_worker_claim(self):
        source,final=complete(); final[TASK+'checks-1.md']=b'worker said pass\nstatus: complete\n'
        with self.assertRaises(MissingEvidence): verify(source,final)
    def test_unapproved_task_state_required(self):
        source,final=complete(); final[TASK+'record.md']=final[TASK+'record.md'].replace(b'state: reviewed',b'state: approved')
        with self.assertRaises(InvalidBehavior): verify(source,final)

if __name__ == '__main__': unittest.main()
