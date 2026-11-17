import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from native import InvalidBehavior, MissingEvidence
from test_native import examples
from target_native import correlate_target

class TargetNativeControls(unittest.TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name)
        p,c=examples(); self.rows=[p[1],*c[1:]]
        for index,rows in enumerate(self.rows): (self.root/f'rollout-{index}.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
        self.source={'parents':['a'],'children':['w1']}; self.before={'wall_ns':1,'monotonic_ns':1}; self.after={'wall_ns':2,'monotonic_ns':2}
        patcher=patch('chronology.within_native'); patcher.start(); self.addCleanup(patcher.stop)
    def run_check(self): return correlate_target(self.root,self.before,self.after,self.source)
    def test_target_only_fresh_lineage(self): self.assertEqual(self.run_check()['children'],['r1','w2','r2'])
    def test_extra_or_missing_child(self):
        (self.root/'rollout-extra.jsonl').write_text('{}\n')
        with self.assertRaises(InvalidBehavior): self.run_check()
        (self.root/'rollout-extra.jsonl').unlink(); (self.root/'rollout-3.jsonl').unlink()
        with self.assertRaises(MissingEvidence): self.run_check()
    def test_linked_log_directory_rejected(self):
        (self.root/'linked').symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(MissingEvidence): self.run_check()

    def test_wrong_model_rejected(self):
        parent = self.rows[0]
        args = json.loads(parent[1]['payload']['arguments']); args['model'] = 'gpt-6.1-terra'
        parent[1]['payload']['arguments'] = json.dumps(args)
        (self.root/'rollout-0.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in parent))
        with self.assertRaises(InvalidBehavior): self.run_check()

    def test_reused_identity_rejected(self):
        self.source['children']=['r1']
        with self.assertRaises(InvalidBehavior): self.run_check()

if __name__ == '__main__': unittest.main()
