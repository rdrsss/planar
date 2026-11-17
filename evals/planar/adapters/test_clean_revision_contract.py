"""No-model rejection controls for the selected native two-step contract."""
from pathlib import Path
import shutil
import tempfile
import unittest
import launch

class CleanContractTest(unittest.TestCase):
    def test_wrong_gate_and_step_verifier_budget_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); candidate=root/'candidate'; case=root/'case'
            shutil.copytree(launch.CANDIDATE,candidate)
            shutil.copytree(launch.CASES['clean-revision'],case,ignore=shutil.ignore_patterns('__pycache__'))
            task=case/'task.toml'; original=task.read_text()
            for changed in (original.replace('min_reward = 1','min_reward = 0'),
                            original.replace('[steps.verifier]\ntimeout_sec = 60','[steps.verifier]\ntimeout_sec = 61',1)):
                task.write_text(changed)
                with self.assertRaisesRegex(ValueError,'step contract'):
                    launch.plan('clean-revision',candidate_root=candidate,case_root=case,trial_deadline=1050)
            task.write_text(original)
            plan=launch.plan('clean-revision',candidate_root=candidate,case_root=case,trial_deadline=1050)
            self.assertEqual(plan['prerequisites']['budgets_seconds']['step_agent'],[400,400])

if __name__=='__main__': unittest.main()
