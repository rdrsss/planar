import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

from recover import recover


class RecoveryControls(unittest.TestCase):
    def test_exact_identity_only_and_confirm_removal(self):
        cid = 'a' * 64
        image = 'sha256:' + 'b' * 64
        current = {'Id': cid, 'Image': image, 'Config': {'Labels': {'com.docker.compose.project': 'smoke-test'}}}
        run = Mock(side_effect=[SimpleNamespace(returncode=0, stdout=json.dumps(current)), SimpleNamespace(returncode=0, stdout=cid), SimpleNamespace(returncode=0, stdout='')])
        recover({'containers': [{'id': cid, 'image_id': image, 'compose_project': 'smoke-test'}]}, run)
        self.assertEqual(run.call_args_list[1].args[0], ['docker', 'rm', '-f', cid])

    def test_mismatched_label_is_never_removed(self):
        cid = 'a' * 64
        image = 'sha256:' + 'b' * 64
        current = {'Id': cid, 'Image': image, 'Config': {'Labels': {'com.docker.compose.project': 'other'}}}
        run = Mock(return_value=SimpleNamespace(returncode=0, stdout=json.dumps(current)))
        with self.assertRaisesRegex(RuntimeError, 'refusing'):
            recover({'containers': [{'id': cid, 'image_id': image, 'compose_project': 'smoke-test'}]}, run)
        self.assertEqual(run.call_count, 1)

    def test_project_cleanup_rejects_wrong_execution_prefix(self):
        import tempfile
        from pathlib import Path
        from recover import recover_projects
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / 'planar-t044-test'
            registry = evidence / 'projects'
            registry.mkdir(parents=True)
            (registry / 'entry.json').write_text(json.dumps({'execution_id': evidence.name, 'compose_project': 'someone-else'}))
            run = Mock()
            with self.assertRaisesRegex(RuntimeError, 'identity'):
                recover_projects(evidence, run)
            run.assert_not_called()
