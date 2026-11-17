"""No-model rejection controls for the selected native two-step contract."""
from pathlib import Path
import json
import shutil
import tempfile
import unittest
from unittest.mock import patch
import launch

def synthetic_pair(root):
    """Match scratch bytes to a scratch case; leave the historical inventory intact."""
    candidate = root / 'candidate'
    manifest = candidate / '.claude-plugin' / 'plugin.json'
    manifest.parent.mkdir(parents=True)
    manifest.write_text('{"name":"synthetic-test-candidate"}\n')
    case = root / 'case'
    shutil.copytree(launch.CASES['clean-revision'], case,
                    ignore=shutil.ignore_patterns('__pycache__'))
    (case / 'candidate.json').write_text(json.dumps(launch.inventory(candidate)))
    return candidate, case


class CleanContractTest(unittest.TestCase):
    def test_candidate_drift_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            candidate, case = synthetic_pair(Path(tmp))
            launch.plan('clean-revision', candidate_root=candidate, case_root=case, trial_deadline=1050)
            (candidate / '.claude-plugin' / 'plugin.json').write_text('{"name":"changed"}\n')
            with self.assertRaisesRegex(ValueError, 'candidate differs from recorded frozen inventory'):
                launch.plan('clean-revision', candidate_root=candidate, case_root=case, trial_deadline=1050)

    def test_wrong_gate_and_step_verifier_budget_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            candidate, case = synthetic_pair(Path(tmp))
            task=case/'task.toml'; original=task.read_text()
            for changed in (original.replace('min_reward = 1','min_reward = 0'),
                            original.replace('[steps.verifier]\ntimeout_sec = 60','[steps.verifier]\ntimeout_sec = 61',1)):
                task.write_text(changed)
                with self.assertRaisesRegex(ValueError,'step contract'):
                    launch.plan('clean-revision',candidate_root=candidate,case_root=case,trial_deadline=1050)
            task.write_text(original)
            plan=launch.plan('clean-revision',candidate_root=candidate,case_root=case,trial_deadline=1050)
            self.assertEqual(plan['prerequisites']['budgets_seconds']['step_agent'],[400,400])
            self.assertEqual(plan['configuration']['agents'][0]['model_name'], 'gpt-6.1-terra')
            self.assertEqual(plan['configuration']['agents'][0]['kwargs']['reasoning_effort'], 'medium')

    def test_plan_rejects_selected_model_or_effort_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            candidate, case = synthetic_pair(Path(tmp))
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
