from pathlib import Path
import unittest

CASE = Path(__file__).resolve().parent
ADAPTERS = CASE.parents[1]/'adapters'

class PackagingTests(unittest.TestCase):
    def test_separate_verifier_copies_match_host_sources(self):
        for name in ('verify_a.py','verify_b.py','assess_worker.py','capture.py','boundary.py','snapshot.py','chronology.py','native.py','projection.json'):
            with self.subTest(name=name):
                self.assertEqual((CASE/name).read_bytes(),(CASE/'tests'/name).read_bytes())
        self.assertEqual((ADAPTERS/'safe_export.py').read_bytes(),(CASE/'tests/safe_export.py').read_bytes())

if __name__ == '__main__': unittest.main()
