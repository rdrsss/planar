from pathlib import Path
import unittest
from native import MissingEvidence, InvalidBehavior
from packets import _required_source, verify_reviewer_packet, TASK


def source_fixture():
    fixture=Path(__file__).resolve().parent.parent/'clean-revision/fixture'
    return {p.relative_to(fixture).as_posix():p.read_bytes() for p in (fixture/'home').rglob('*') if p.is_file()}


def make_packet(source,attempt):
    checks=f'# Checks {attempt}\nactual check details\nstatus: complete\n'
    prior='# Review: T-001, attempt 1, all\nF1: actual finding\nverdict: request-changes\nstatus: complete\n'
    state='fault-injected' if attempt==1 else 'correct'
    diff=''.join(f'diff --git a/{name} b/{name}\nnew file mode 100644\n--- /dev/null\n+++ b/{name}\n@@ -0,0 +1 @@\n+{value}\n' for name,value in [('RESULT.txt','2'),('STATE.txt',state)])
    text='\n\n'.join(_required_source(source))+f'\n\n## Diff\n{diff}\n## Checks\n{checks}'
    if attempt==2: text+='\n## Earlier review\n'+prior
    text+='\n## Unattributed changes\nNone\n\n## Configuration\ncodex profiles/revision-fault.yaml ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7 gpt-6.1-sol/medium\n\nstatus: complete\n'
    final={TASK+f'checks-{attempt}.md':checks.encode(),TASK+f'reviewer-packet-{attempt}.md':text.encode(),TASK+'review-1-all.md':prior.encode()}
    return final

class PacketControls(unittest.TestCase):
    def setUp(self): self.source=source_fixture()
    def test_both_full_packets(self):
        for n in (1,2): self.assertEqual(verify_reviewer_packet(self.source,make_packet(self.source,n),n)['semantic_audit'],'required')
    def test_missing_full_requirement_or_check_is_incomplete(self):
        for n in (1,2):
            final=make_packet(self.source,n); key=TASK+f'reviewer-packet-{n}.md'; final[key]=final[key].replace(b'RESULT.txt contains exactly `2`',b'RESULT.txt contains exactly `3`')
            with self.assertRaises(MissingEvidence): verify_reviewer_packet(self.source,final,n)
            final=make_packet(self.source,n); final[key]=final[key].replace(b'actual check details',b'summary')
            with self.assertRaises(MissingEvidence): verify_reviewer_packet(self.source,final,n)
    def test_full_testing_spec_and_profile_binding_required(self):
        for token in (b'TEST-C1 Exact result.',b'ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7'):
            final=make_packet(self.source,1); key=TASK+'reviewer-packet-1.md'; final[key]=final[key].replace(token,b'absent')
            with self.subTest(token=token), self.assertRaises(MissingEvidence): verify_reviewer_packet(self.source,final,1)

    def test_earlier_review_must_be_full(self):
        final=make_packet(self.source,2); key=TASK+'reviewer-packet-2.md'; final[key]=final[key].replace(b'F1: actual finding',b'F1 summary')
        with self.assertRaises(MissingEvidence): verify_reviewer_packet(self.source,final,2)
    def test_wrong_diff_and_unattributed_path(self):
        final=make_packet(self.source,1); key=TASK+'reviewer-packet-1.md'; final[key]=final[key].replace(b'+fault-injected',b'+correct')
        with self.assertRaises(InvalidBehavior): verify_reviewer_packet(self.source,final,1)
        final=make_packet(self.source,1); final[key]=final[key].replace(b'a/STATE.txt b/STATE.txt',b'a/OTHER.txt b/OTHER.txt')
        with self.assertRaises(InvalidBehavior): verify_reviewer_packet(self.source,final,1)
    def test_unattributed_account_and_config_required(self):
        for before in (b'## Unattributed changes',b'gpt-6.1-sol/medium'):
            final=make_packet(self.source,1); key=TASK+'reviewer-packet-1.md'; final[key]=final[key].replace(before,b'missing')
            with self.assertRaises(MissingEvidence): verify_reviewer_packet(self.source,final,1)

if __name__ == '__main__': unittest.main()
