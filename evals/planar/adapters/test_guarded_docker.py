import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from harbor.models.task.config import EnvironmentConfig, NetworkMode, NetworkPolicy
from harbor.models.trial.paths import TrialPaths
from guarded_docker import GuardedDocker

CASES = Path(__file__).resolve().parents[1] / 'cases'


class VerifierCaseGuard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.evidence = Path(self.tmp.name) / ('planar-t044-noauth-' + 'a' * 32)
        self.evidence.mkdir()
        self.env = patch.dict(os.environ, {'PLANAR_T044_EVIDENCE_DIR': str(self.evidence)}, clear=True)
        self.env.start(); self.addCleanup(self.env.stop)

    def construct(self, case, directory, mode=NetworkMode.NO_NETWORK):
        os.environ['PLANAR_T044_CASE'] = case
        return GuardedDocker(environment_dir=directory, environment_name='verifier', session_id='trial__verifier__',
            trial_paths=TrialPaths(self.evidence / 'trial'), task_env_config=EnvironmentConfig(),
            network_policy=NetworkPolicy(network_mode=mode), auto_mounting=False)

    def test_supported_cases_construct_and_register(self):
        for case in ('plugin-smoke', 'native-lineage-smoke', 'fresh-interrupted-review'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                self.evidence = Path(tmp) / ('planar-t044-noauth-' + 'b' * 32); self.evidence.mkdir()
                os.environ['PLANAR_T044_EVIDENCE_DIR'] = str(self.evidence)
                env = self.construct(case, CASES / case / 'tests')
                self.assertTrue(env._static_verifier)
                self.assertFalse(env.auto_mounting)
                self.assertFalse(env._host_mounts)
                self.assertEqual(len(list((self.evidence / 'projects').glob('*.json'))), 1)

    def test_wrong_case_directory_unsupported_and_network_rejected(self):
        for case, directory, mode in (
            ('native-lineage-smoke', CASES / 'plugin-smoke/tests', NetworkMode.NO_NETWORK),
            ('plugin-smoke', CASES / 'native-lineage-smoke/tests', NetworkMode.NO_NETWORK),
            ('../../other', CASES / 'plugin-smoke/tests', NetworkMode.NO_NETWORK),
            ('native-lineage-smoke', CASES / 'native-lineage-smoke/tests', NetworkMode.PUBLIC),
        ):
            with self.subTest(case=case, directory=directory, mode=mode):
                with self.assertRaises(ValueError): self.construct(case, directory, mode)
        self.assertFalse((self.evidence / 'projects').exists())


if __name__ == '__main__': unittest.main()
