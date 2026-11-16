import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from check_provenance import check


class ProvenanceControls(unittest.TestCase):
    def fixture(self, directory):
        def write(name, data):
            (directory / name).write_text(json.dumps(data))
        run_id = directory.name
        config = directory / 'job-config.json'
        config.write_text('{}')
        common = {'execution_id': run_id}
        write('launcher.json', {**common, 'candidate_sha256': 'candidate', 'job_config_sha256': hashlib.sha256(config.read_bytes()).hexdigest()})
        write('planar-image.json', {**common, 'id': 'sha256:' + 'a' * 64})
        write('planar-cli.json', {**common, 'version': 'codex-cli 0.169.0'})
        write('planar-install-provenance.json', {**common, 'candidate_sha256': 'candidate', 'plugin_id': 'planar@planar-smoke-market', 'installed_path': '/installed-agent/codex-home/plugins/cache/planar-smoke-market/planar/0.1.0', 'list_contains_plugin': True})
        write('planar-launch.json', {**common, 'cli_version': '0.169.0', 'model': 'gpt-6.1-terra', 'command': 'codex exec --json'})
        write('planar-terminal.json', {**common, 'status': 'shell_completed', 'return_code': 0})
        write('planar-cleanup.json', {**common, 'status': 'passed'})
        write('launcher-terminal.json', {**common, 'status': 'completed'})

    def test_consistent_host_provenance_remains_inconclusive(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.fixture(directory)
            self.assertEqual(check(directory)['status'], 'inconclusive')
            (directory / 'job-config.json').write_text('{"changed": true}')
            with self.assertRaisesRegex(ValueError, 'configuration identity'):
                check(directory)

    def test_wrong_execution_and_missing_cleanup_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.fixture(directory)
            file = directory / 'planar-cleanup.json'
            file.write_text(json.dumps({'execution_id': 'different', 'status': 'passed'}))
            with self.assertRaisesRegex(ValueError, 'identity'):
                check(directory)
            file.unlink()
            with self.assertRaisesRegex(ValueError, 'absent'):
                check(directory)
