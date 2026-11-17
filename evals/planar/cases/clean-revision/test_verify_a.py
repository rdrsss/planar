import json
from pathlib import Path
import tempfile
import unittest
from capture import save
from test_assess_worker import example, retained_aliases, retained_live
from assess_worker import TASK
from test_native import examples, rebase
from verify_a import assess
from verify_b import assess as assess_b

class VerifyFirstTests(unittest.TestCase):
    def test_positive_and_missing_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            case = Path(__file__).resolve().parent
            fixture = case.parent/'fresh-interrupted-review/fixture-v2'
            projection = json.loads((case/'projection.json').read_text())['files']
            initial = {name:(fixture/name).read_bytes() for name in projection}
            initial['host/git-status.txt'] = b''
            initial['host/git-head.txt'] = b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n'
            save(root/'initial', initial)
            pre = dict(initial); pre.update(retained_aliases())
            save(root/'pre', pre)
            logs = root/'logs/worker/sessions/2026/01/01'; logs.mkdir(parents=True)
            parents, children = examples()
            initial_time = json.loads((root/'initial-receipt.json').read_text())['wall_ns']
            for name, rows in (('parent',rebase(parents[0],initial_time,0)),('child',rebase(children[0],initial_time,0))):
                (logs/('rollout-'+name+'.jsonl')).write_text(''.join(json.dumps(row)+'\n' for row in rows))
            self.assertEqual(assess(root)['status'],'pass')
            self.assertEqual(assess_b(root)['status'], 'incomplete')
            (logs/'rollout-child.jsonl').unlink()
            self.assertEqual(assess(root)['status'],'incomplete')

    def test_actual_live_a_through_shared_b(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            case = Path(__file__).resolve().parent
            fixture = case.parent/'fresh-interrupted-review/fixture-v2'
            projection = json.loads((case/'projection.json').read_text())['files']
            initial = {name:(fixture/name).read_bytes() for name in projection}
            initial['host/git-status.txt'] = b''
            initial['host/git-head.txt'] = b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n'
            save(root/'initial', initial)
            pre = dict(initial); pre.update(retained_live())
            save(root/'pre', pre)
            logs = root/'logs/worker/sessions/2026/01/01'; logs.mkdir(parents=True)
            parents, children = examples()
            initial_time = json.loads((root/'initial-receipt.json').read_text())['wall_ns']
            for name, rows in (('parent',rebase(parents[0],initial_time,0)),('child',rebase(children[0],initial_time,0))):
                (logs/('rollout-'+name+'.jsonl')).write_text(''.join(json.dumps(row)+'\n' for row in rows))
            self.assertEqual(assess(root)['status'],'pass')
            self.assertEqual(assess_b(root)['status'], 'incomplete')
            (logs/'rollout-child.jsonl').unlink()
            self.assertEqual(assess(root)['status'],'incomplete')

    def test_shared_b_rejects_conflicting_alias_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); case = Path(__file__).resolve().parent
            fixture = case.parent/'fresh-interrupted-review/fixture-v2'
            projection = json.loads((case/'projection.json').read_text())['files']
            initial = {name:(fixture/name).read_bytes() for name in projection}
            initial['host/git-status.txt'] = b''
            initial['host/git-head.txt'] = b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n'
            save(root/'initial', initial)
            pre = dict(initial); changed = retained_aliases()
            key = TASK+'session-I-002-1.md'
            changed[key] = changed[key].replace(b'Invocation ID: I-002', b'Invocation ID: I-002; invocation: I-003')
            pre.update(changed); save(root/'pre', pre)
            self.assertEqual(assess(root)['status'], 'behavioral_failure')
            self.assertEqual(assess_b(root)['status'], 'behavioral_failure')

if __name__ == '__main__': unittest.main()
