import hashlib
import json
from pathlib import Path
from unittest import TestCase
from unittest.mock import patch
import retained_b_early_trace as trace
import launch

PATH=launch.PROJECT/'reviews/t045-retained-b-final-prep-20261117/output/jobs/planar-t044-aeaec469c5c549ee96f3ce7729dc4c2e/retained-b/logs/revision/codex.txt'

class EarlyTrace(TestCase):
    def setUp(self):
        if not PATH.is_file(): self.skipTest('retained original trace unavailable')
        self.raw=PATH.read_bytes(); lines=self.raw.decode().splitlines(); self.events=[json.loads(line) for line in lines[1:]]
    def modified(self, events):
        raw=('Reading additional input from stdin...\n'+'\n'.join(json.dumps(e) for e in events)+'\n').encode()
        with patch.object(trace,'TRACE_SHA256',hashlib.sha256(raw).hexdigest()): return trace.inspect_trace(raw)
    def test_original_exact_audited_trace(self):
        self.assertEqual(trace.inspect_trace(self.raw)['target_children'],0)
    def test_changed_raw_hash_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'trace bytes'): trace.inspect_trace(self.raw+b' ')
    def test_missing_completion_extra_child_or_reorder_rejected(self):
        events=self.events
        for changed in (events[:-1],events[:-1]+[{'type':'item.completed','item':{'id':'item_14','type':'tool_call'}}]+events[-1:],events[:-2]+[events[-1],events[-2]]):
            with self.subTest(length=len(changed)),self.assertRaises(ValueError): self.modified(changed)
