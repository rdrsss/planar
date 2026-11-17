"""No-auth control for the selected case's one-use gate ordering."""
import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import launch

class GateOrderTest(unittest.TestCase):
    def test_fixed_budget_is_required(self):
        with self.assertRaisesRegex(ValueError, '1050-second'):
            launch.plan('clean-revision', trial_deadline=1000)
        with self.assertRaisesRegex(ValueError, '1050-second'):
            launch.plan('clean-revision', trial_deadline=1050, effort='high')

    def test_gate_consumed_before_auth_path_access(self):
        image = 'sha256:' + 'a' * 64
        snapshot = {'implementation_sha256':'impl', 'candidate_sha256':'candidate',
                    'configuration_sha256':'config', 'case_sha256':'case', 'image_id':image}
        gate = {'verdict':'approve', 'purpose':'one-diagnostic-attempt', 'max_attempts':1,
                **snapshot, 'case':'clean-revision', 'verifier_image_id':image, 'absolute_deadline':'2100-01-01T00:00:00+00:00'}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            review = root / 'review.json'
            review.write_text(json.dumps(gate))
            case = root / 'case'; case.mkdir()
            jobs = root / 'jobs'; jobs.mkdir()
            task = SimpleNamespace(config=SimpleNamespace(environment=SimpleNamespace(docker_image=image), verifier=SimpleNamespace(environment=SimpleNamespace(docker_image=image))))
            result = SimpleNamespace(returncode=0, stdout=image+'\n')
            with patch.object(launch, 'plan', return_value=snapshot), \
                 patch.object(launch, 'reviews_root', return_value=root), \
                 patch.object(launch, 'jobs_root', return_value=jobs), \
                 patch.object(launch, 'Task', return_value=task), \
                 patch.object(launch.shutil, 'which', return_value='/usr/bin/docker'), \
                 patch.object(launch.subprocess, 'run', return_value=result):
                with self.assertRaises(FileNotFoundError):
                    launch.execute(root/'missing-auth', review, 'clean-revision', case_root=case, candidate_root=root, trial_deadline=1050, absolute_deadline=gate['absolute_deadline'])
            markers = list((root/'harbor-t044-consumed').iterdir())
            self.assertEqual(len(markers), 1)

if __name__ == '__main__': unittest.main()
