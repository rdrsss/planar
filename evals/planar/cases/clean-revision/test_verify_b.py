import hashlib
import json
from pathlib import Path
import tempfile
import time
import unittest
from capture import save
from test_assess_worker import example, CASE
from test_native import examples, rebase
from verify_b import assess, TASK, CHECK


def prepare(root):
    fixture = CASE.parent/'fresh-interrupted-review/fixture-v2'
    projection = json.loads((CASE/'projection.json').read_text())['files']
    initial = {name:(fixture/name).read_bytes() for name in projection}
    initial.update({'host/git-status.txt':b'', 'host/git-head.txt':b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n'})
    save(root/'initial', initial)
    pre = dict(initial); pre.update(example())
    def inventory(state):
        return json.dumps({'files':{'STATE.txt':{'sha256':hashlib.sha256(state).hexdigest(),'size':len(state)},'README.md':{'sha256':'unchanged'}},'directories':['.git']},sort_keys=True).encode()
    pre['host/repository-inventory.json']=inventory(b'correct\n')
    save(root/'pre', pre)
    started = {'wall_ns':time.time_ns(), 'monotonic_ns':time.monotonic_ns()}
    post = dict(pre); post['repository/STATE.txt']=b'fault-injected\n'; post['host/repository-inventory.json']=inventory(b'fault-injected\n')
    ended = {'wall_ns':time.time_ns(), 'monotonic_ns':time.monotonic_ns()}
    save(root/'post', post)
    (root/'injection.json').write_text(json.dumps({'started':started,'ended':ended}))
    final = dict(pre)
    record = pre[TASK+'record.md'].decode().replace('state: worker-done','state: reviewed').replace('attempt: 1\nbase:', 'attempt: 2\nbase:')
    additions = [('checks-done',1,'checks-1.md'), ('review-packet-rendered',1,'reviewer-packet-1.md'), ('reviewed',1,'review-1-all.md: request-changes'), ('changes-requested',1,'review-1-all.md'), ('packet-rendered',2,'worker-packet-2.md'), ('worker-done',2,'worker-report-2.md'), ('checks-done',2,'checks-2.md'), ('review-packet-rendered',2,'reviewer-packet-2.md'), ('reviewed',2,'review-2-all.md: approve')]
    for step,attempt,detail in additions:
        record += f'- 2026-01-01 00:01 | {step} | attempt {attempt} | session unavailable/unavailable | skill | {detail}\n'
    final[TASK+'record.md'] = record.encode()
    for n in (1,2):
        final[TASK+f'checks-{n}.md']=f'# Checks: T-001, attempt {n}\n\n## TEST-C1\n\nResult: passed\nCommand: `{CHECK}`\nExit code: 0\n\nstatus: complete\n'.encode()
        final[TASK+f'reviewer-packet-{n}.md']=b'packet\nstatus: complete\n'
    finding='- **F1** cites: PRD-R2. STATE.txt is fault-injected; correct it.'
    final[TASK+'review-1-all.md']=(f'# Review: T-001, attempt 1, all\n\n## Findings\n\n{finding}\n\nverdict: request-changes\nstatus: complete\n').encode()
    final[TASK+'worker-packet-2.md']=(f'# Revision\n\n{finding}\n\nstatus: complete\n').encode()
    final[TASK+'worker-report-2.md']=b'# Worker\n\nstatus: done\n'
    final[TASK+'review-2-all.md']=b'# Review: T-001, attempt 2, all\n\nverdict: approve\nstatus: complete\n'
    save(root/'final',final)
    parents,children=examples()
    first = json.loads((root/'initial-receipt.json').read_text())['wall_ns']
    second = json.loads((root/'post-receipt.json').read_text())['wall_ns']
    for folder,rows in [('worker', [rebase(parents[0],first,0),rebase(children[0],first,0)]),('revision',[rebase(parents[1],second,15),*[rebase(child,second,15) for child in children[1:]]])]:
        directory = root/'logs'/folder/'sessions/2026/01/01'; directory.mkdir(parents=True)
        for i,events in enumerate(rows):
            (directory/f'rollout-{i}.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in events))
    return final

class RevisionTests(unittest.TestCase):
    def test_complete_synthetic_and_missing_native(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); prepare(root)
            self.assertEqual(assess(root)['status'],'pass')
            (root/'logs/revision/sessions/2026/01/01/rollout-3.jsonl').unlink()
            self.assertEqual(assess(root)['status'],'incomplete')
    def test_note_alongside_actionable_finding(self):
        finding = '- **F1** cites: PRD-R2. STATE.txt is fault-injected; correct it.'
        note = '- **F2** (note) cites: PRD-R1. RESULT.txt already has required bytes; no change requested.'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); final=prepare(root)
            import shutil
            shutil.rmtree(root/'final'); (root/'final-receipt.json').unlink()
            final[TASK+'review-1-all.md'] = (f'# Review: T-001, attempt 1, all\n\n## Findings\n\n{finding}\n\n{note}\n\nverdict: request-changes\nstatus: complete\n').encode()
            final[TASK+'worker-packet-2.md'] = (f'# Revision\n\n{finding}\n\n{note}\n\nstatus: complete\n').encode()
            save(root/'final',final)
            self.assertEqual(assess(root)['status'],'pass')
            shutil.rmtree(root/'final'); (root/'final-receipt.json').unlink()
            final[TASK+'worker-packet-2.md'] = (f'# Revision\n\n{finding}\n\nstatus: complete\n').encode()
            save(root/'final',final)
            self.assertEqual(assess(root)['status'],'behavioral_failure')

    def test_malformed_note_and_note_only_fail(self):
        finding = '- **F1** cites: PRD-R2. STATE.txt is fault-injected; correct it.'
        for entries in (finding + '\n\n- **F2** (notes) cites: TS-1. Observation.',
                        '- **F1** (note) cites: PRD-R2. STATE.txt is fault-injected; correct it.'):
            with self.subTest(entries=entries), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); final=prepare(root)
                import shutil
                shutil.rmtree(root/'final'); (root/'final-receipt.json').unlink()
                final[TASK+'review-1-all.md'] = (f'# Review: T-001, attempt 1, all\n\n## Findings\n\n{entries}\n\nverdict: request-changes\nstatus: complete\n').encode()
                final[TASK+'worker-packet-2.md'] = (f'# Revision\n\n{entries}\n\nstatus: complete\n').encode()
                save(root/'final',final)
                self.assertEqual(assess(root)['status'],'behavioral_failure')

    def test_missing_or_reordered_host_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); prepare(root)
            (root/'post-receipt.json').unlink()
            self.assertEqual(assess(root)['status'],'incomplete')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); prepare(root)
            path=root/'post-receipt.json'; data=json.loads(path.read_text())
            data['wall_ns']=1; path.write_text(json.dumps(data))
            self.assertEqual(assess(root)['status'],'behavioral_failure')
    def test_missing_previous_finding_and_wrong_final(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); final=prepare(root)
            import shutil
            shutil.rmtree(root/'final'); (root/'final-receipt.json').unlink(); final[TASK+'worker-packet-2.md']=b'no finding\n'; save(root/'final',final)
            self.assertEqual(assess(root)['status'],'behavioral_failure')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); final=prepare(root)
            import shutil
            shutil.rmtree(root/'final'); (root/'final-receipt.json').unlink(); final['repository/STATE.txt']=b'fault-injected\n'; save(root/'final',final)
            self.assertEqual(assess(root)['status'],'behavioral_failure')

if __name__ == '__main__': unittest.main()
