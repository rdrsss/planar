import tempfile
import hashlib
import json
import unittest
from pathlib import Path
from assess import assess, assess_export, unassessed_extras, TASK
from install_fixture import install
from test_native import examples

class IntegratedDraft(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name); install(self.root)
        task=self.root/TASK
        record=task/'record.md'
        record.write_text(record.read_text().replace('state: review-packet-rendered', 'state: reviewed', 1))
        with record.open('a') as f:
            f.write('- 2026-11-16 10:30 | started | attempt 1 | session unavailable/unavailable | skill | invocation reviewer-2; predecessor reviewer-1; session-reviewer-2-1.md\n')
            f.write('- 2026-11-16 10:31 | session-observed | attempt 1 | session unavailable/unavailable | skill | invocation reviewer-2; session-reviewer-2-2.md\n')
            f.write('- 2026-11-16 10:32 | reviewed | attempt 1 | reviewer gpt-6.1-sol/medium | skill | review-1-all.md: request-changes; evidence=session-reviewer-2-2.md\n')
        (task/'session-reviewer-2-1.md').write_text('task: T-001; attempt: 1; invocation: reviewer-2; phase: started; reviewer-packet-1-replacement-1.md; base: 08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n')
        (task/'session-reviewer-2-2.md').write_text('task: T-001; attempt: 1; invocation: reviewer-2; phase: completed; review-1-all.md\n')
        (task/'review-1-all.md').write_text('# Review: T-001, attempt 1, all\n\n## Findings\n\n- F1 cites: PRD-R2. STATE.txt is fault-injected but must be correct.\n\nverdict: request-changes\nstatus: complete\n')
        self.parent,self.child=examples()
    def install_direct_preflight(self):
        task=self.root/TASK
        packet=task/'reviewer-packet-1.md'
        hashes=[hashlib.sha256(packet.read_bytes()).hexdigest()]
        for path in (self.root/'repository/RESULT.txt', self.root/'repository/STATE.txt', task/'checks-1.md', packet, self.root/'home/profiles/revision-fault.yaml', *(self.root/'home/projects/revision-fault'/name for name in ('prd.md','tech-spec.md','layout.md','testing-spec.md','tasks.md'))):
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest())
        (task/'packet-preflight-1.md').write_text('T-001 base 08bacc341a31f32c14a1695b5b2de5c44b3a3e56; RESULT.txt STATE.txt checks profile configuration; sources read in full and verified.\n'+'\n'.join(hashes[:6]))
        record=task/'record.md'; data=record.read_text()
        marker='- 2026-11-16 10:30 | started'
        row='- 2026-11-16 10:29 | session-observed | attempt 1 | session unavailable/unavailable | skill | packet-preflight-1.md\n'
        record.write_text(data.replace(marker,row+marker,1))
        start=task/'session-reviewer-2-1.md'
        start.write_text(start.read_text().replace('reviewer-packet-1-replacement-1.md','reviewer-packet-1.md'))
        reviews=self.root/'home/projects/revision-fault/reviews'
        (reviews/'cleanup-resume-20261116-1.md').write_text('owned invocation reviewer-2\n')
        with record.open('a') as f: f.write('- 2026-11-16 10:33 | session-observed | attempt 1 | session unavailable/unavailable | skill | reviews/cleanup-resume-20261116-1.md\n')
    def use_canonical_retry(self):
        task=self.root/TASK
        old_start=task/'session-reviewer-2-1.md'; old_done=task/'session-reviewer-2-2.md'
        (task/'session-reviewer-1-retry-1.md').write_text(old_start.read_text().replace('reviewer-2','reviewer-1-retry'))
        (task/'session-reviewer-1-retry-2.md').write_text('invocation: reviewer-1-retry; phase: accepted\n')
        (task/'session-reviewer-1-retry-3.md').write_text(old_done.read_text().replace('reviewer-2','reviewer-1-retry'))
        old_start.unlink(); old_done.unlink()
        (task/'packet-preflight-1.md').rename(task/'preflight-review-1-replacement-1.md')
        record=task/'record.md'; data=record.read_text().replace('reviewer-2','reviewer-1-retry').replace('packet-preflight-1.md','preflight-review-1-replacement-1.md')
        data=data.replace('invocation reviewer-1-retry; session-reviewer-1-retry-2.md', 'invocation reviewer-1-retry accepted; session-reviewer-1-retry-2.md\n- 2026-11-16 10:31 | session-observed | attempt 1 | session unavailable/unavailable | skill | invocation reviewer-1-retry completed; session-reviewer-1-retry-3.md')
        data=data.replace('evidence=session-reviewer-1-retry-2.md','evidence=session-reviewer-1-retry-3.md')
        record.write_text(data)
        reviews=self.root/'home/projects/revision-fault/reviews'
        (reviews/'run-status.md').write_text('synthetic status\n')
        (reviews/'cleanup-resume-20261116-1.md').write_text('owned invocation reviewer-1-retry\n')
    def use_preflight_session_and_arbitrary_retry(self):
        task=self.root/TASK
        preflight=task/'preflight-review-1-replacement-1.md'
        (task/'session-review-preflight-1-1.md').write_text('invocation: review-preflight-1; phase: complete\n'+preflight.read_text())
        preflight.unlink()
        for sequence in (1,2,3):
            old=task/f'session-reviewer-1-retry-{sequence}.md'
            new=task/f'session-reviewer-retry-1-{sequence}.md'
            text=old.read_text().replace('reviewer-1-retry','reviewer-retry-1')
            if sequence == 1: text += 'predecessor: reviewer-1\n'
            new.write_text(text); old.unlink()
        record=task/'record.md'; data=record.read_text().replace('reviewer-1-retry','reviewer-retry-1')
        data=data.replace('preflight-review-1-replacement-1.md','invocation review-preflight-1; session-review-preflight-1-1.md')
        data=data.replace('predecessor reviewer-1; session-reviewer-retry-1-1.md','session-reviewer-retry-1-1.md')
        record.write_text(data)
        cleanup=self.root/'home/projects/revision-fault/reviews/cleanup-resume-20261116-1.md'
        cleanup.write_text(cleanup.read_text().replace('reviewer-1-retry','reviewer-retry-1'))

    def test_finding_requires_supported_citation_and_actual_defect(self):
        self.install_direct_preflight(); path=self.root/TASK/'review-1-all.md'; original=path.read_text()
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        path.write_text(original.replace('PRD-R2','TS-1'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        path.write_text(original.replace('PRD-R2','PRD-R1, PRD-R2'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        for value in (original.replace('PRD-R2','PRD-R1'),
                      original.replace('F1 cites: PRD-R2. STATE.txt is fault-injected but must be correct.',
                                       '**F1** cites: PRD-R1. RESULT.txt is wrong. Unrelated tokens: PRD-R2 STATE.txt fault-injected correct.'),
                      original.replace('STATE.txt is fault-injected but must be correct.',''),
                      original.replace('F1 cites:', 'note cites:'),
                      original.replace('F1 cites: PRD-R2. STATE.txt is fault-injected but must be correct.',
                                       'F1 cites: PRD-R2. \n\nSTATE.txt fault-injected correct')):
            with self.subTest(value=value):
                path.write_text(value)
                self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_preflight_candidate_and_packet_hashes_are_required(self):
        self.install_direct_preflight(); task=self.root/TASK; path=task/'packet-preflight-1.md'; original=path.read_text()
        for source in (self.root/'repository/STATE.txt', task/'reviewer-packet-1.md'):
            with self.subTest(source=source.name):
                digest=hashlib.sha256(source.read_bytes()).hexdigest()
                path.write_text(original.replace(digest,'0'*64))
                self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        path.write_text(original)
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')

    def test_cleanup_label_and_bounded_lifecycle(self):
        self.install_direct_preflight(); self.use_canonical_retry(); self.use_preflight_session_and_arbitrary_retry()
        record=self.root/TASK/'record.md'; original=record.read_text()
        record.write_text(original.replace('skill | reviews/cleanup-', 'skill | cleanup reviews/cleanup-'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        labelled=record.read_text()
        for extra in ('- now | approved | attempt 1 | reviewer x/y | skill | review-1-all.md: approve\n',
                      '- now | started | attempt 1 | worker x/y | skill | invocation worker-2; extra\n',
                      '- now | session-observed | attempt 1 | session x/y | skill | cleanup reviews/cleanup-extra-1.md\n'):
            with self.subTest(extra=extra):
                record.write_text(labelled+extra)
                self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        record.write_text(labelled)
        (self.root/'home/projects/revision-fault/reviews/cleanup-resume-20261116-1.md').unlink()
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')

    def test_base_from_linked_observation_and_packet(self):
        self.install_direct_preflight(); task=self.root/TASK
        path=task/'packet-preflight-1.md'; base='08bacc341a31f32c14a1695b5b2de5c44b3a3e56'
        path.write_text(path.read_text().replace(base,'HEAD matches base'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        start=task/'session-reviewer-2-1.md'; original=start.read_text()
        start.write_text(original.replace('base: '+base, 'base: '+('f'*40)))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        start.write_text(original.replace('; base: '+base, ''))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')
        start.write_text(original+'; base: '+('f'*40)+'\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        start.write_text(original)
        record=task/'record.md'; record_original=record.read_text()
        record.write_text(record_original.replace('base: '+base, 'base: '+base+'\nbase: '+('f'*40), 1))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        record.write_text(record_original)
        preflight=path.read_text()
        path.write_text(preflight+'\nbase: '+('f'*40)+'\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        path.write_text(preflight.replace('HEAD matches base','unstructured observation'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')

    def test_cleanup_links_and_ownership_are_bounded(self):
        self.install_direct_preflight(); task=self.root/TASK; record=task/'record.md'; original=record.read_text()
        path='reviews/cleanup-resume-20261116-1.md'
        for replacement in ('', path+'; '+path, path+'; /other/'+path, '/other/'+path, '../'+path, '%2e%2e/'+path, 'projects/other/'+path):
            with self.subTest(replacement=replacement):
                record.write_text(original.replace(path,replacement))
                self.assertNotEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        record.write_text(original)
        cleanup=self.root/'home/projects/revision-fault'/path
        cleanup.write_text('owned invocation reviewer-unrelated')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_source_and_approval_bytes_remain_bound(self):
        self.install_direct_preflight(); project=self.root/'home/projects/revision-fault'
        for name in ('prd.md','project.yaml'):
            path=project/name; data=path.read_bytes()
            with self.subTest(name=name):
                path.write_bytes(data+b'!')
                self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
                path.write_bytes(data)

    def test_conflicting_linked_observation_identities(self):
        self.install_direct_preflight(); task=self.root/TASK
        start=task/'session-reviewer-2-1.md'; original=start.read_text()
        for text,expected in ((original+'; invocation: other\n','behavioral_failure'),
                              (original.replace('task: T-001; ','') ,'incomplete'),
                              (original.replace('attempt: 1','attempt: 2'),'behavioral_failure')):
            with self.subTest(text=text):
                start.write_text(text); self.assertEqual(assess(self.root,self.parent,self.child)[0],expected)

    def test_explicit_completion_base_conflict(self):
        self.install_direct_preflight(); completion=self.root/TASK/'session-reviewer-2-2.md'
        original=completion.read_text(); self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        completion.write_text(original+'base: '+('f'*40)+'\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        completion.write_text(original+'base: unavailable\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        completion.write_text(original+'base: 08bacc3\nbase: 08bacc3\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_explicit_cleanup_owner_fields_cannot_conflict(self):
        self.install_direct_preflight(); cleanup=self.root/'home/projects/revision-fault/reviews/cleanup-resume-20261116-1.md'
        cleanup.write_text('owned invocation: reviewer-unrelated\nreference: reviewer-2\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        for field in ('task: T-002','attempt: 2','invocation: reviewer-unrelated'):
            with self.subTest(field=field):
                cleanup.write_text('owned invocation: reviewer-2\n'+field+'\n')
                self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        cleanup.write_text('owned invocation: reviewer-2\ntask: unavailable\nattempt: unavailable\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')

    def test_owned_collaboration_handle_requires_bound_native_chain(self):
        self.install_direct_preflight(); self.use_canonical_retry()
        task=self.root/TASK; cleanup=self.root/'home/projects/revision-fault/reviews/cleanup-resume-20261116-1.md'
        accepted=task/'session-reviewer-1-retry-2.md'; completed=task/'session-reviewer-1-retry-3.md'
        accepted.write_text('task: T-001; attempt: 1; invocation: reviewer-1-retry; phase: dispatch accepted; collaboration agent name: /root/reviewer\n')
        completed.write_text(completed.read_text()+'collaboration agent name: /root/reviewer\n')
        cleanup.write_text('/root/reviewer, owned by this session for independent review\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        original=accepted.read_text()
        accepted.write_text(original.replace('/root/reviewer','/root/other'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        accepted.write_text(original.replace('/root/reviewer','unavailable'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')
        accepted.write_text(original.replace('task: T-001; ',''))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')
        accepted.write_text(original)
        cleanup.write_text('/root/other, owned by this session; reference /root/reviewer\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        cleanup.write_text('reference: /root/reviewer\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')
        cleanup.write_text('owned invocation: reviewer-1-retry; owned handle: /root/other\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        cleanup.write_text('owned invocation: reviewer-1-retry; owned handle: /outside/other\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
        cleanup.write_text('owned invocation: reviewer-1-retry; owned handle: unavailable\n')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')

    def test_bad_verdict(self):
        self.install_direct_preflight()
        path=self.root/TASK/'review-1-all.md'; path.write_text(path.read_text().replace('request-changes','approve'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_direct_valid_packet_recovery(self):
        self.install_direct_preflight(); self.use_canonical_retry(); self.use_preflight_session_and_arbitrary_retry()
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        self.assertFalse((self.root/TASK/'reviewer-packet-1-replacement-1.md').exists())

    def test_missing_and_changed_evidence(self):
        self.install_direct_preflight(); task=self.root/TASK
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        (task/'checks-1.md').write_bytes(b'x')
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_original_malformed_packet_cannot_pass_v2(self):
        self.install_direct_preflight(); task=self.root/TASK
        old=Path(__file__).resolve().parent/'fixture/home/projects/revision-fault/tasks/T-001/reviewer-packet-1.md'
        (task/'reviewer-packet-1.md').write_bytes(old.read_bytes())
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_bad_record_tail_transitions(self):
        self.install_direct_preflight(); record=self.root/TASK/'record.md'; original=record.read_text()
        for extra in ('reviewed | attempt 1 | reviewer gpt-6.1-sol/medium | skill | review-0-all.md: approve',
                      'started | attempt 1 | worker x/y | skill | invocation worker-2; extra',
                      'started | attempt 1 | session unavailable/unavailable | skill | invocation checks-2; extra',
                      'approved | attempt 1 | reviewer x/y | skill | review-1-all.md: approve'):
            with self.subTest(extra=extra):
                record.write_text(original+'- now | '+extra+'\n')
                self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_completed_checks_cannot_be_redispatched_before_review(self):
        self.install_direct_preflight(); task=self.root/TASK
        record=task/'record.md'; text=record.read_text()
        extra='- now | started | attempt 1 | session unavailable/unavailable | skill | invocation checks-2; session-checks-2-1.md\n'
        (task/'session-checks-2-1.md').write_text('synthetic observation')
        record.write_text(text.replace('- 2026-11-16 10:29 | session-observed',extra+'- 2026-11-16 10:29 | session-observed',1))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')

    def test_missing_native_completion(self):
        self.install_direct_preflight(); self.child.pop()
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')

    def test_preflight_prose_and_missing_source(self):
        self.install_direct_preflight(); task=self.root/TASK; record=task/'record.md'
        record.write_text(record.read_text().replace('skill | packet-preflight-1.md','skill | review preflight packet-preflight-1.md'))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'pass')
        (task/'packet-preflight-1.md').unlink()
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')

    def test_unlinked_extra_has_no_credit(self):
        self.install_direct_preflight(); task=self.root/TASK
        (task/'other-note.md').write_text((task/'packet-preflight-1.md').read_text())
        self.assertIn(str(TASK/'other-note.md'),unassessed_extras(self.root))
        (task/'packet-preflight-1.md').unlink()
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'incomplete')

    def test_invalid_observation_link(self):
        self.install_direct_preflight(); record=self.root/TASK/'record.md'
        record.write_text(record.read_text().replace('session-reviewer-2-1.md','../session-reviewer-2-1.md',1))
        self.assertEqual(assess(self.root,self.parent,self.child)[0], 'behavioral_failure')
