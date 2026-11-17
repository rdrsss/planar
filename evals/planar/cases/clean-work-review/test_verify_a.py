import json
from pathlib import Path
import tempfile
import unittest
from capture import save
from test_assess_worker import example
from test_native import examples, rebase
from verify_a import assess


class FirstBoundaryTests(unittest.TestCase):
    def test_complete_snapshot_and_missing_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); case=Path(__file__).parent
            projection=json.loads((case/'projection.json').read_text())['files']
            initial={name:(case/'fixture'/name).read_bytes() for name in projection}
            initial.update({'host/git-status.txt':b'', 'host/git-head.txt':b'08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n'})
            save(root/'initial',initial)
            pre=dict(initial); pre.update(example()); pre['home/projects/revision-fault/tasks.md']=initial['home/projects/revision-fault/tasks.md']
            save(root/'pre',pre)
            logs=root/'logs/worker/sessions/2026/01/01'; logs.mkdir(parents=True)
            parents,children=examples(); start=json.loads((root/'initial-receipt.json').read_text())['wall_ns']
            for name, rows in (('parent',rebase(parents[0],start,0)),('child',rebase(children[0],start,0))):
                (logs/('rollout-'+name+'.jsonl')).write_text(''.join(json.dumps(row)+'\n' for row in rows))
            self.assertEqual(assess(root)['status'],'pass')
            (logs/'rollout-child.jsonl').unlink()
            self.assertEqual(assess(root)['status'],'incomplete')
