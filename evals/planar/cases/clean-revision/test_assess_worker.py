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

if __name__ == '__main__': unittest.main()
