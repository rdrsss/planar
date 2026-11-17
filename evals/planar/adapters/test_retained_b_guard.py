import os
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from harbor.models.task.config import EnvironmentConfig, NetworkMode, NetworkPolicy
from harbor.models.trial.paths import TrialPaths
from guarded_docker import GuardedDocker

class RetainedGuardControls(unittest.TestCase):
    def construct(self,evidence):
        case=Path(__file__).resolve().parents[1]/'cases/retained-a-fresh-b/tests'
        return GuardedDocker(environment_dir=case,environment_name='verifier',session_id='trial__verifier__main',
            trial_paths=TrialPaths(evidence/'trial'),task_env_config=EnvironmentConfig(),network_policy=NetworkPolicy(network_mode=NetworkMode.NO_NETWORK),auto_mounting=False)
    def test_rejects_without_explicit_noauth_and_with_any_auth(self):
        for flags in ({},{'PLANAR_T045_NOAUTH_CONTROL':'1','CODEX_AUTH_JSON_PATH':'forbidden'}, {'PLANAR_T045_NOAUTH_CONTROL':'1','OPENAI_API_KEY':'forbidden'}):
            with self.subTest(flags=flags),TemporaryDirectory() as tmp:
                evidence=Path(tmp).resolve()/('planar-t044-noauth-'+'b'*32); evidence.mkdir()
                with patch.dict(os.environ,{'PLANAR_T044_EVIDENCE_DIR':str(evidence),'PLANAR_T044_CASE':'retained-a-fresh-b',**flags},clear=True):
                    with self.assertRaises(ValueError): self.construct(evidence)
    def test_noauth_separate_verifier_constructs(self):
        with TemporaryDirectory() as tmp:
            evidence=Path(tmp).resolve()/('planar-t044-noauth-'+'b'*32); evidence.mkdir()
            with patch.dict(os.environ,{'PLANAR_T044_EVIDENCE_DIR':str(evidence),'PLANAR_T044_CASE':'retained-a-fresh-b','PLANAR_T045_NOAUTH_CONTROL':'1'},clear=True):
                self.assertTrue(self.construct(evidence)._static_verifier)

if __name__=='__main__': unittest.main()
