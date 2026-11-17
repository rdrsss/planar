import tempfile
import unittest
from pathlib import Path
from step_identity import identify, load_instructions, validate_selection

CASE = Path(__file__).resolve().parent

class StepIdentityTests(unittest.TestCase):
    def test_exact_host_instruction(self):
        frozen = load_instructions(CASE)
        self.assertEqual(identify(frozen['worker'][0], frozen), 'worker')
        self.assertEqual(identify(frozen['revision'][0], frozen), 'revision')
        with self.assertRaises(ValueError): identify(frozen['worker'][0] + ' ', frozen)
        altered = dict(frozen); altered['revision'] = (frozen['revision'][0], '0'*64)
        with self.assertRaises(ValueError): identify(frozen['revision'][0], altered)

    def test_both_statements_bind_actual_configured_selection(self):
        frozen = load_instructions(CASE)
        statement = validate_selection(frozen, 'gpt-6.1-terra', 'medium')
        self.assertIn('configured selection', statement)
        self.assertIn('not independently observed', statement)
        for model, effort in (('gpt-6.1-sol','medium'), ('gpt-6.1-terra','high'), ('gpt-6.1-terra',None)):
            with self.subTest(model=model, effort=effort), self.assertRaisesRegex(ValueError, 'configured selection'):
                validate_selection(frozen,model,effort)
        for name in ('worker','revision'):
            altered = dict(frozen); altered[name] = (frozen[name][0].replace(statement,''), frozen[name][1])
            with self.subTest(step=name), self.assertRaisesRegex(ValueError, 'instruction selection'):
                validate_selection(altered,'gpt-6.1-terra','medium')

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
