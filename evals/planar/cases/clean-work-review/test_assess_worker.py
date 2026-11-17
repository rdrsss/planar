from pathlib import Path
import unittest
from assess_worker import assess_worker
from links import TASK, Incomplete, Contradiction


def example():
    root = Path(__file__).parent/'fixtures/retained-a'
    files = {TASK+p.name:p.read_bytes() for p in root.iterdir() if p.is_file()}
    files[TASK+'session-I-001-2.md'] = files[TASK+'session-I-001-1.md'].replace(b'phase: started', b'phase: preflight')
    files.update({'host/git-head.txt':b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n', 'repository/RESULT.txt':b'2\n', 'repository/STATE.txt':b'correct\n',
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


class PortableObservedForms(unittest.TestCase):
    def observed_forms(self):
        # Exercise the observed syntax using the repository's synthetic fixture.
        # The separately retained original run and its regrade are unchanged.
        files = example()
        for key in list(files):
            if key.startswith(TASK):
                files[key] = files[key].replace(b'I-001', b'render-1').replace(b'I-002', b'worker-1')
        for old in list(files):
            new = old.replace('session-I-001-', 'session-render-1-').replace('session-I-002-', 'session-worker-1-')
            if new != old:
                files[new] = files.pop(old)
        record = TASK+'record.md'
        files[record] = files[record].replace(b'base: 08bacc341a31f32c14a1695b5b2de5c44b3a3e56', b'base: 08bacc3')
        files[record] = files[record].replace(b'worker-report-1.md: done;', b'worker-report-1.md: done; session-worker-1-3.md;')
        for key in list(files):
            if key.startswith(TASK+'session-'):
                files[key] = (files[key].replace(b'base: 08bacc341a31f32c14a1695b5b2de5c44b3a3e56', b'base: 08bacc3.')
                              .replace(b'timestamp:', b'time:')
                              .replace(b'parent: render-1;', b'parent: render-1 (same driver);')
                              .replace(b'phase: completed\n', b'phase: complete.\n'))
        return files

    def test_observed_abbreviation_and_structured_observations(self):
        self.assertEqual(assess_worker(self.observed_forms())['boundary'],'worker-done')

    def test_wrong_base_parent_and_ambiguous_links_reject(self):
        for change in ('wrong_record_base','too_short','wrong_note_base','wrong_host_head','wrong_parent','duplicate_wrong_link','unzoned_time'):
            files=self.observed_forms()
            if change=='wrong_record_base': files[TASK+'record.md']=files[TASK+'record.md'].replace(b'base: 08bacc3',b'base: 08bacc4',1)
            elif change=='too_short': files[TASK+'record.md']=files[TASK+'record.md'].replace(b'base: 08bacc3',b'base: 08bacc',1)
            elif change=='wrong_note_base': files[TASK+'session-worker-1-2.md']=files[TASK+'session-worker-1-2.md'].replace(b'base: 08bacc3',b'base: 08bacc4',1)
            elif change=='wrong_host_head': files['host/git-head.txt']=b'wrong\n'
            elif change=='wrong_parent': files[TASK+'session-worker-1-2.md']=files[TASK+'session-worker-1-2.md'].replace(b'parent: render-1 (same driver)',b'parent: other (same driver)',1)
            elif change=='duplicate_wrong_link': files[TASK+'record.md']+=b'- 2026-11-17 12:32 | reviewed | attempt 1 | session | skill | session-worker-1-3.md\n'
            else: files[TASK+'session-worker-1-2.md']=files[TASK+'session-worker-1-2.md'].replace(b'+00:00; phase:',b'; phase:',1)
            with self.subTest(change=change),self.assertRaises((Incomplete,Contradiction)): assess_worker(files)
