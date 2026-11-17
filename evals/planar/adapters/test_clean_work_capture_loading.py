import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import ModuleType
from unittest import TestCase
from unittest.mock import patch
from clean_revision_support import load_case_module

CASE=Path(__file__).resolve().parents[1]/'cases/clean-work-review'

class CaptureCaseIdentity(TestCase):
    def test_save_after_loader_returns_ignores_cached_generic_modules(self):
        bogus=ModuleType('boundary'); bogus.manifest=lambda _: (_ for _ in ()).throw(AssertionError('wrong helper'))
        with patch.dict('sys.modules',{'boundary':bogus}):
            capture=load_case_module(CASE,'capture')
            with TemporaryDirectory() as tmp:
                target=Path(tmp)/'pre'; capture.save(target,{'repository/STATE.txt':b'correct\n'})
                self.assertEqual(capture.load(target),{'repository/STATE.txt':b'correct\n'})
                self.assertEqual(json.loads((Path(tmp)/'pre-receipt.json').read_text())['stage'],'pre')
                self.assertEqual(Path(capture._boundary.__file__).resolve(),(CASE/'boundary.py').resolve())
                self.assertEqual(Path(capture._snapshot.__file__).resolve(),(CASE/'snapshot.py').resolve())
                self.assertEqual(Path(capture._exports.__file__).resolve(),(CASE/'safe_export.py').resolve())
