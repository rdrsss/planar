from pathlib import Path
import unittest

CASE=Path(__file__).resolve().parent
OLD=CASE.parent/'clean-revision'
ADAPTERS=CASE.parents[1]/'adapters'

class PackageControls(unittest.TestCase):
    def test_verifier_copies_match_fixed_sources(self):
        for name in ('source_packet.py','source-policy.json','verify_restore.py','target_native.py','target_verifier.py','review_structure.py','packets.py','check_evidence.py','check_binding.py','delivery.py'):
            with self.subTest(name=name): self.assertEqual((CASE/name).read_bytes(),(CASE/'tests'/name).read_bytes())
        for name in ('verify_a.py','assess_worker.py','capture.py','chronology.py','native.py','boundary.py','snapshot.py','projection.json'):
            with self.subTest(name=name): self.assertEqual((OLD/name).read_bytes(),(CASE/'tests'/name).read_bytes())
        for name in ('safe_export.py','candidate.py'):
            with self.subTest(name=name): self.assertEqual((ADAPTERS/name).read_bytes(),(CASE/'tests'/name).read_bytes())

if __name__=='__main__': unittest.main()
