import json
from pathlib import Path
import tempfile
import unittest
from handoff import worker_gate

class HandoffTests(unittest.TestCase):
    def test_only_archived_success_authorizes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'run'; root.mkdir()
            (root/'native-trial-start.json').write_text(json.dumps({'trial_name':'case__abc','job_dir':str(root/'run')}))
            reward = root/'run/case__abc/steps/worker/verifier/reward.txt'
            reward.parent.mkdir(parents=True); reward.write_text('1\n')
            self.assertEqual(worker_gate(root)['reward'],'1')
            reward.write_text('0\n')
            with self.assertRaisesRegex(ValueError,'did not pass'): worker_gate(root)
            reward.unlink()
            with self.assertRaisesRegex(ValueError,'missing'): worker_gate(root)
    def test_reject_foreign_trial_and_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'run'; root.mkdir()
            start = root/'native-trial-start.json'
            start.write_text(json.dumps({'trial_name':'../other','job_dir':str(root/'run')}))
            with self.assertRaisesRegex(ValueError,'identity'): worker_gate(root)
            start.write_text(json.dumps({'trial_name':'case','job_dir':str(root/'run')}))
            (root/'run').symlink_to(Path(tmp),target_is_directory=True)
            with self.assertRaisesRegex(ValueError,'unsafe'): worker_gate(root)

if __name__ == '__main__': unittest.main()
