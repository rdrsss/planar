"""The verifier artifact must not contain privately retained rejected bytes."""
import asyncio
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch
from guarded_docker import GuardedDocker

class PrivateExportTest(unittest.TestCase):
    def test_rejected_archive_and_receipt_are_not_sent_to_verifier(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); clean=root/'clean-revision'; (clean/'logs/worker').mkdir(parents=True)
            (clean/'active.json').write_text(json.dumps({'step':'worker'}))
            (clean/'container.json').write_text(json.dumps({'container_id':'a'*64,'baseline':['init']}))
            (clean/'rejected-home.tar').write_bytes(b'synthetic private data')
            (clean/'rejected-home.json').write_text('{}')
            (clean/'initial-receipt.json').write_text('{}')
            env=GuardedDocker.__new__(GuardedDocker); env._static_verifier=False
            env._case_name='clean-revision'; env._evidence_root=root
            env._platform=SimpleNamespace(_resolve_service_container=AsyncMock(return_value='container'))
            fake=SimpleNamespace(collect=AsyncMock(return_value={'home/settings.yaml':b'fixed'}),save=Mock())
            target=root/'artifact'
            with patch('clean_revision_support.load_case_module',return_value=fake), patch('guarded_docker.subprocess.run',return_value=SimpleNamespace(stdout='a'*64+'\n')):
                asyncio.run(env.download_dir('/workspace/home',str(target)))
            self.assertTrue((target/'_planar_host/initial-receipt.json').exists())
            self.assertFalse((target/'_planar_host/rejected-home.tar').exists())
            self.assertFalse((target/'_planar_host/rejected-home.json').exists())

if __name__=='__main__': unittest.main()
