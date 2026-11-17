"""No-model rejection controls for the selected native two-step contract."""
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
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

    def test_plan_rejects_selected_model_or_effort_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); candidate=root/'candidate'; case=root/'case'
            shutil.copytree(launch.CANDIDATE,candidate)
            shutil.copytree(launch.CASES['clean-revision'],case,ignore=shutil.ignore_patterns('__pycache__'))
            original=launch.config_for
            for field,value in (('model_name','gpt-6.1-sol'),('reasoning_effort','high')):
                def altered(*args, **kwargs):
                    config=original(*args, **kwargs)
                    if field == 'model_name': config['agents'][0]['model_name']=value
                    else: config['agents'][0]['kwargs']['reasoning_effort']=value
                    return config
                with self.subTest(field=field), patch.object(launch,'config_for',side_effect=altered):
                    with self.assertRaisesRegex(ValueError,'configured selection'):
                        launch.plan('clean-revision',candidate_root=candidate,case_root=case,trial_deadline=1050)

if __name__=='__main__': unittest.main()
