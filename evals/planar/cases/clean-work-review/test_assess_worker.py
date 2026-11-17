from pathlib import Path
import unittest
from assess_worker import assess_worker
from links import TASK, Incomplete, Contradiction


def example():
    root = Path(__file__).parent/'fixtures/retained-a'
    files = {TASK+p.name:p.read_bytes() for p in root.iterdir() if p.is_file()}
    files[TASK+'session-I-001-2.md'] = files[TASK+'session-I-001-1.md'].replace(b'phase: started', b'phase: preflight')
    files.update({'repository/RESULT.txt':b'2\n', 'repository/STATE.txt':b'correct\n',
                  'home/projects/revision-fault/tasks.md':b'- [ ] **T-001** Create output\n'})
    return files


class WorkerTests(unittest.TestCase):
    def test_record_links_without_invocation_prefix(self):
        files = example()
        files[TASK+'record.md'] = files[TASK+'record.md'].replace(b'I-002: session-I-002-',b'observation session-I-002-')
        self.assertEqual(assess_worker(files)['boundary'], 'worker-done')
    def test_cross_task_missing_or_orphan_observation_fails(self):
        for change in ('task', 'missing', 'orphan'):
            files = example(); key=TASK+'session-I-002-2.md'
            if change == 'task': files[key] = files[key].replace(b'Task: T-001',b'Task: T-002')
            elif change == 'missing': del files[key]
            else: files[TASK+'record.md'] = b'\n'.join(line for line in files[TASK+'record.md'].split(b'\n') if b'session-I-002-2.md' not in line)
            with self.subTest(change=change), self.assertRaises((Incomplete, Contradiction)):
                assess_worker(files)
    def test_wrong_product_or_check_before_worker_boundary_fails(self):
        for key,value in (('repository/STATE.txt',b'bad\n'), (TASK+'checks-1.md',b'check')):
            files=example(); files[key]=value
            with self.assertRaises(Contradiction): assess_worker(files)
