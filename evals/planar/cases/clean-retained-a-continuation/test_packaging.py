from pathlib import Path
import unittest


class Packaging(unittest.TestCase):
    def test_verifier_copy_matches_case(self):
        case=Path(__file__).parent
        for name in ('verify_b.py','source_packet.py','target_native.py','native.py','chronology.py','no_fault.py',
                     'capture.py','boundary.py','safe_export.py','snapshot.py','links.py','check_evidence.py',
                     'delivery.py','reviewer_packet.py','source-policy.json'):
            with self.subTest(name=name): self.assertEqual((case/name).read_bytes(),(case/'tests'/name).read_bytes())
