from pathlib import Path
from unittest import TestCase

CASE=Path(__file__).resolve().parent

class Packaging(TestCase):
    def test_separate_verifier_sources_match_case(self):
        for name in ('verify_a.py','verify_b.py','assess_worker.py','capture.py','boundary.py','snapshot.py','chronology.py','native.py','projection.json','safe_export.py','links.py','no_fault.py','reviewer_packet.py','check_evidence.py','delivery.py'):
            with self.subTest(name=name): self.assertEqual((CASE/name).read_bytes(),(CASE/'tests'/name).read_bytes())
