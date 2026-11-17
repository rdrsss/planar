"""No-auth controls for case-selected verifier image and provenance checks."""
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
from guarded_docker import GuardedDocker
from harbor.environments.docker.docker import DockerEnvironment

class VerifierImageTest(unittest.TestCase):
    def test_wrong_image_fails_and_correct_image_has_step_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = GuardedDocker.__new__(GuardedDocker)
            env._static_verifier=True; env._case_name='clean-revision'; env._verifier_step='worker'
            env._platform=SimpleNamespace(_resolve_service_container=AsyncMock(return_value='container'))
            fake=[SimpleNamespace(stdout='none\n'),SimpleNamespace(stdout='sha256:'+('b'*64)+'\n')]
            settings={'PLANAR_T044_EVIDENCE_DIR':tmp,'PLANAR_T045_VERIFIER_IMAGE_ID':'sha256:'+('a'*64)}
            with patch.dict('os.environ',settings,clear=True), patch.object(DockerEnvironment,'start',new=AsyncMock()), patch('guarded_docker.subprocess.run',side_effect=fake):
                with self.assertRaisesRegex(ValueError,'differs from gate'):
                    asyncio.run(env.start(False))
            fake=[SimpleNamespace(stdout='none\n'),SimpleNamespace(stdout='sha256:'+('a'*64)+'\n')]
            with patch.dict('os.environ',settings,clear=True), patch.object(DockerEnvironment,'start',new=AsyncMock()), patch('guarded_docker.subprocess.run',side_effect=fake):
                asyncio.run(env.start(False))
            receipt=json.loads((Path(tmp)/'verifier-network-worker.json').read_text())
            self.assertEqual(receipt['step'],'worker')
            self.assertEqual(receipt['image_id'],'sha256:'+('a'*64))

if __name__=='__main__': unittest.main()
