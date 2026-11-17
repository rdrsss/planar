import os
import json
import shutil
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from harbor.models.task.config import EnvironmentConfig, NetworkMode, NetworkPolicy
from harbor.models.trial.paths import TrialPaths
from guarded_docker import GuardedDocker
import launch
from smoke_codex import SmokeCodex

CASES = Path(__file__).resolve().parents[1] / 'cases'


class VerifierCaseGuard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.evidence = Path(self.tmp.name).resolve() / ('planar-t044-noauth-' + 'a' * 32)
        self.evidence.mkdir()
        self.env = patch.dict(os.environ, {'PLANAR_T044_EVIDENCE_DIR': str(self.evidence)}, clear=True)
        self.env.start(); self.addCleanup(self.env.stop)

    def construct(self, case, directory, mode=NetworkMode.NO_NETWORK):
        os.environ['PLANAR_T044_CASE'] = case
        return GuardedDocker(environment_dir=directory, environment_name='verifier', session_id='trial__verifier__',
            trial_paths=TrialPaths(self.evidence / 'trial'), task_env_config=EnvironmentConfig(),
            network_policy=NetworkPolicy(network_mode=mode), auto_mounting=False)

    def test_supported_cases_construct_and_register(self):
        for case in launch.CASES:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                self.evidence = Path(tmp).resolve() / ('planar-t044-noauth-' + 'b' * 32); self.evidence.mkdir()
                os.environ['PLANAR_T044_EVIDENCE_DIR'] = str(self.evidence)
                env = self.construct(case, CASES / case / 'tests')
                with patch('smoke_codex.jobs_root', return_value=self.evidence.parent):
                    agent = SmokeCodex(logs_dir=self.evidence / 'logs', version='0.169.0')
                self.assertEqual(agent._case_name, case)
                self.assertTrue(env._static_verifier)
                self.assertFalse(env.auto_mounting)
                self.assertFalse(env._host_mounts)
                self.assertEqual(len(list((self.evidence / 'projects').glob('*.json'))), 1)

    def test_snapshot_binding_and_negatives(self):
        case = 'native-lineage-current'
        root = Path(self.tmp.name).resolve() / 'case-snapshot'
        shutil.copytree(CASES / case, root)
        digest = launch.case_inventory(root)
        os.environ.update(PLANAR_T044_CASE_SNAPSHOT=str(root), PLANAR_T044_CASE_SHA256=digest,
                          PLANAR_T044_CANDIDATE_SNAPSHOT=str(Path(self.tmp.name).resolve() / 'candidate'))
        record = {'execution_id': self.evidence.name, 'case': case, 'case_sha256': digest,
                  'prerequisites': {'case_read_root': str(root)}}
        (self.evidence / 'launcher.json').write_text(json.dumps(record))
        for directory, mode in ((CASES / case / 'tests', NetworkMode.NO_NETWORK),
                                (root / 'tests', NetworkMode.PUBLIC)):
            with self.assertRaises(ValueError): self.construct(case, directory, mode)
        link = Path(self.tmp.name).resolve() / 'linked'; link.symlink_to(root, target_is_directory=True)
        os.environ['PLANAR_T044_CASE_SNAPSHOT'] = str(link)
        with self.assertRaises(ValueError): self.construct(case, link / 'tests')
        os.environ['PLANAR_T044_CASE_SNAPSHOT'] = str(root)
        with self.assertRaises(ValueError): self.construct('plugin-smoke', root / 'tests')
        (root / 'instruction.md').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'identity mismatch'): self.construct(case, root / 'tests')
        shutil.copyfile(CASES / case / 'instruction.md', root / 'instruction.md')
        env = self.construct(case, root / 'tests')
        self.assertTrue(env._static_verifier)

    def test_agent_rejects_unknown_case(self):
        os.environ['PLANAR_T044_CASE'] = 'unknown'
        with patch('smoke_codex.jobs_root', return_value=self.evidence.parent):
            with self.assertRaisesRegex(ValueError, 'unsupported case'):
                SmokeCodex(logs_dir=self.evidence / 'logs', version='0.169.0')

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
