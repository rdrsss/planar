from pathlib import Path
import unittest
from assess_worker import assess_worker, Incomplete, Contradiction, TASK

CASE = Path(__file__).resolve().parent

def example():
    fixture = CASE.parent/'fresh-interrupted-review/fixture-v2/home/projects/revision-fault'
    record = (fixture/'tasks/T-001/record.md').read_text().split('\n- 2026-11-16 06:39 | started |',1)[0]
    record = record.replace('state: review-packet-rendered', 'state: worker-done')+'\n'
    return {TASK+'record.md':record.encode(), TASK+'worker-report-1.md':b'# Work\n\nstatus: done\n',
            TASK+'worker-packet-1.md':b'packet',
            TASK+'session-packet-1-1.md':(fixture/'tasks/T-001/session-packet-1-1.md').read_bytes(),
            TASK+'session-worker-1-1.md':(fixture/'tasks/T-001/session-worker-1-1.md').read_bytes(),
            TASK+'session-worker-1-2.md':(fixture/'tasks/T-001/session-worker-1-2.md').read_bytes(),
            'repository/RESULT.txt':b'2\n', 'repository/STATE.txt':b'correct\n',
            'home/projects/revision-fault/tasks.md':(fixture/'tasks.md').read_bytes()}

class WorkerBoundaryTests(unittest.TestCase):
    def test_valid(self): self.assertEqual(assess_worker(example())['boundary'], 'worker-done')
    def test_check_and_wrong_state_rejected(self):
        files = example(); files[TASK+'checks-1.md'] = b'pass'
        with self.assertRaises(Contradiction): assess_worker(files)
        files = example(); files['repository/STATE.txt'] = b'fault-injected\n'
        with self.assertRaises(Contradiction): assess_worker(files)
    def test_missing_report_incomplete(self):
        files = example(); del files[TASK+'worker-report-1.md']
        with self.assertRaises(Incomplete): assess_worker(files)
    def test_duplicate_field_and_tick_rejected(self):
        files = example(); files[TASK+'record.md'] = files[TASK+'record.md'].replace(b'state: worker-done', b'state: worker-done\nstate: reviewed')
        with self.assertRaises(Contradiction): assess_worker(files)
        files = example(); files['home/projects/revision-fault/tasks.md'] = files['home/projects/revision-fault/tasks.md'].replace(b'- [ ] **T-001**',b'- [x] **T-001**')
        with self.assertRaises(Contradiction): assess_worker(files)

def retained():
    files = example()
    for name in list(files):
        if name.startswith(TASK): del files[name]
    for path in (CASE/'fixtures/retained-a').iterdir():
        files[TASK+path.name] = path.read_bytes()
    return files

def retained_aliases():
    files = retained()
    driver = TASK+'session-I-001-1.md'
    files[driver] = (files[driver].replace(b'step: packet-rendered', b'step: render worker packet')
                     .replace(b'Invocation: I-001', b'Invocation ID: I-001')
                     .replace(b'phase: started', b'phase: start'))
    for number in (1, 2, 3):
        key = TASK+f'session-I-002-{number}.md'
        files[key] = (files[key].replace(b'step: worker-done', b'step: worker dispatch')
                      .replace(b'invocation: I-002', b'Invocation ID: I-002' if number == 1 else b'invocation: I-002')
                      .replace(b'phase: started', b'phase: start')
                      .replace(b'phase: dispatch accepted', b'phase: accepted')
                      .replace(b'phase: completed', b'phase: completed (child return)'))
    return files

class RetainedBoundaryTests(unittest.TestCase):
    def test_actual_record_and_observations(self):
        self.assertEqual(assess_worker(retained())['boundary'], 'worker-done')

    def test_canonical_combined_aliases_and_duplicate_keys(self):
        files = retained_aliases()
        self.assertEqual(assess_worker(files)['boundary'], 'worker-done')
        key = TASK+'session-I-002-1.md'
        files[key] = files[key].replace(b'Invocation ID: I-002', b'Invocation ID: I-002; invocation: I-003')
        with self.assertRaises(Contradiction): assess_worker(files)

    def test_unknown_alias_is_incomplete(self):
        for key, before, after in ((TASK+'session-I-001-1.md', b'step: render worker packet', b'step: unknown'),
                                   (TASK+'session-I-002-2.md', b'phase: accepted', b'phase: unknown'),
                                   (TASK+'session-I-002-3.md', b'phase: completed (child return)', b'phase: completed (other)'),
                                   (TASK+'session-I-002-1.md', b'Invocation ID:', b'Identity:')):
            files = retained_aliases(); files[key] = files[key].replace(before, after)
            with self.subTest(key=key), self.assertRaises(Incomplete): assess_worker(files)

    def test_annotated_completion_at_start_is_conflicting(self):
        files = retained_aliases(); key = TASK+'session-I-002-1.md'
        files[key] = files[key].replace(b'phase: start', b'phase: completed (child return)')
        with self.assertRaises(Contradiction): assess_worker(files)

    def test_acceptance_after_annotated_completion_conflicts(self):
        files = retained_aliases(); accept = TASK+'session-I-002-2.md'; complete = TASK+'session-I-002-3.md'
        files[accept] = files[accept].replace(b'phase: accepted', b'phase: completed (child return)')
        files[complete] = files[complete].replace(b'phase: completed (child return)', b'phase: accepted')
        with self.assertRaises(Contradiction): assess_worker(files)

    def test_missing_and_duplicate_links(self):
        for before, after in ((b'I-002: session-I-002-1.md', b'no-start-link'),
                              (b'I-002: session-I-002-3.md', b'no-completion-link'),
                              (b'I-002: session-I-002-3.md', b'I-002: session-I-002-3.md; I-002: session-I-002-3.md')):
            files = retained(); key = TASK+'record.md'; files[key] = files[key].replace(before, after)
            with self.assertRaises((Incomplete, Contradiction)): assess_worker(files)

    def test_orphan_acceptance_and_shared_wrong_parent(self):
        files = retained(); key = TASK+'record.md'
        files[key] = b'\n'.join(line for line in files[key].split(b'\n') if b'I-002: session-I-002-2.md' not in line)
        with self.assertRaises((Incomplete, Contradiction)): assess_worker(files)
        files = retained()
        for number in (1, 2, 3):
            key = TASK+f'session-I-002-{number}.md'
            files[key] = files[key].replace(b'parent: I-001', b'parent: I-009')
        with self.assertRaises(Contradiction): assess_worker(files)

    def test_conflicting_observation_identity_and_result(self):
        changes = [(b'Task: T-001', b'Task: T-002'), (b'attempt: 1', b'attempt: 2'),
                   (b'role: worker', b'role: reviewer'), (b'invocation: I-002', b'invocation: I-003'),
                   (b'parent: I-001', b'parent: I-009'), (b'worker-report-1.md, SHA-256', b'other.md, SHA-256'),
                   (b'status: done', b'status: failed'), (b'phase: completed', b'phase: started')]
        for before, after in changes:
            files = retained(); key = TASK+'session-I-002-3.md'; files[key] = files[key].replace(before, after)
            with self.subTest(change=before), self.assertRaises((Incomplete, Contradiction)): assess_worker(files)
        files = retained(); key = TASK+'worker-report-1.md'; files[key] += b'mutated'
        with self.assertRaises(Contradiction): assess_worker(files)

if __name__ == '__main__': unittest.main()
