import tempfile
import unittest
from pathlib import Path
from step_identity import identify, load_instructions

CASE = Path(__file__).resolve().parent

class StepIdentityTests(unittest.TestCase):
    def test_exact_host_instruction(self):
        frozen = load_instructions(CASE)
        self.assertEqual(identify(frozen['worker'][0], frozen), 'worker')
        self.assertEqual(identify(frozen['revision'][0], frozen), 'revision')
        with self.assertRaises(ValueError): identify(frozen['worker'][0] + ' ', frozen)
        altered = dict(frozen); altered['revision'] = (frozen['revision'][0], '0'*64)
        with self.assertRaises(ValueError): identify(frozen['revision'][0], altered)

    def test_identical_instructions_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('worker', 'revision'):
                path = root / 'steps' / name
                path.mkdir(parents=True)
                (path / 'instruction.md').write_text('same\n')
            with self.assertRaisesRegex(ValueError, 'ambiguous'):
                load_instructions(root)

if __name__ == '__main__': unittest.main()
