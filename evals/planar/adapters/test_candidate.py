import tempfile
import unittest
from pathlib import Path

from candidate import inventory


class CandidateInventoryTests(unittest.TestCase):
    def test_content_and_paths_affect_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'one').write_bytes(b'original')
            original = inventory(root)
            self.assertEqual(original, inventory(root))
            (root / 'one').write_bytes(b'changed')
            self.assertNotEqual(original['sha256'], inventory(root)['sha256'])
            (root / 'one').rename(root / 'two')
            self.assertNotEqual(original['sha256'], inventory(root)['sha256'])

    def test_empty_and_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(ValueError):
                inventory(root)
            (root / 'link').symlink_to('/etc/passwd')
            with self.assertRaises(ValueError):
                inventory(root)


if __name__ == '__main__':
    unittest.main()
