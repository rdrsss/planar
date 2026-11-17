import os
import shutil
import subprocess
import sys
from pathlib import Path
from unittest import TestCase
from unittest.mock import patch
import launch


class PortableNativePath(TestCase):
    def test_actual_restricted_child_path_finds_inspected_binaries(self):
        docker=shutil.which('docker'); codex=shutil.which('codex')
        if not docker or not codex: self.skipTest('host Docker and Codex unavailable')
        expected=subprocess.run([str(Path(codex).resolve()),'--version'],capture_output=True,text=True,check=True,env={'PATH':os.defpath}).stdout.strip()
        path=launch.portable_native_path(docker,expected)
        code="import os,shutil; from pathlib import Path; print(Path(shutil.which('docker')).resolve()); print(Path(shutil.which('codex')).resolve()); print('AUTH_ABSENT',not any('AUTH' in k or 'KEY' in k for k in os.environ))"
        child=subprocess.run([sys.executable,'-c',code],env={'PATH':path},capture_output=True,text=True,check=True)
        self.assertEqual(child.stdout.splitlines(),[str(Path(docker).resolve()),str(Path(codex).resolve()),'AUTH_ABSENT True'])
    def test_missing_codex_and_wrong_version_fail_closed(self):
        with patch.object(launch.shutil,'which',return_value=None):
            with self.assertRaisesRegex(ValueError,'Codex executable unavailable'): launch.portable_native_path('/missing/docker','codex-cli 0.169.0')
        docker=shutil.which('docker'); codex=shutil.which('codex')
        if not docker or not codex: return
        with self.assertRaisesRegex(ValueError,'version mismatch'): launch.portable_native_path(docker,'codex-cli impossible')
