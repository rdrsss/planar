import asyncio
import json
import unittest
from unittest.mock import AsyncMock, patch
import capture


def snapshot(state=b'correct\n'):
    return {'repository/STATE.txt':state, 'repository/RESULT.txt':b'2\n',
            'host/repository-inventory.json':json.dumps({'files':{'STATE.txt':{'sha256':state.hex()},'README.md':{'sha256':'base'}},'directories':['.git']}).encode(),
            'host/git-status.txt':b'?? STATE.txt\n'}

class CaptureTests(unittest.TestCase):
    def test_reject_nonquiescent_before_write(self):
        with patch.object(capture,'census',return_value=['other']), patch.object(capture,'_docker') as command:
            with self.assertRaisesRegex(ValueError,'quiescent'):
                asyncio.run(capture.inject(None,'a'*64,['expected'],snapshot()))
            command.assert_not_called()
    def test_reject_wrong_original_before_write(self):
        with patch.object(capture,'census',return_value=['expected']), patch.object(capture,'_docker') as command:
            with self.assertRaisesRegex(ValueError,'initial state'):
                asyncio.run(capture.inject(None,'a'*64,['expected'],snapshot(b'wrong')))
            command.assert_not_called()
    def test_non_state_metadata_change_fails(self):
        after = snapshot(b'fault-injected\n')
        meta = json.loads(after['host/repository-inventory.json']); meta['files']['README.md']['sha256'] = 'altered'
        after['host/repository-inventory.json'] = json.dumps(meta).encode()
        with patch.object(capture,'census',return_value=['expected']), patch.object(capture,'_docker'), patch.object(capture,'collect',new=AsyncMock(return_value=after)):
            with self.assertRaisesRegex(ValueError,'metadata delta'):
                asyncio.run(capture.inject(None,'a'*64,['expected'],snapshot()))

if __name__ == '__main__': unittest.main()

class SavedSnapshotTests(unittest.TestCase):
    def test_roundtrip_and_tampering(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'snapshot'
            files = {'repository/STATE.txt':b'correct\n', 'host/git-status.txt':b'status'}
            capture.save(path,files)
            self.assertEqual(capture.load(path),files)
            (path/'repository/STATE.txt').write_bytes(b'wrong')
            with self.assertRaisesRegex(ValueError,'content mismatch'): capture.load(path)
    def test_added_entry_and_symlink_rejected(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'snapshot'
            capture.save(path,{'repository/STATE.txt':b'correct\n'})
            (path/'unexpected').write_text('x')
            with self.assertRaisesRegex(ValueError,'inventory mismatch'): capture.load(path)
