import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import source_account


class SourceAccount(unittest.TestCase):
    def test_locked_consumer_is_scoped_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); reviews=root/'reviews'; scope=reviews/'output/planar-account-admission-v1'; scope.mkdir(parents=True)
            packet=root/'packet'; (packet/'evidence').mkdir(parents=True)
            (packet/'evidence/original-gate.json').write_text(json.dumps({'account_scope_id':str(scope),'execution_id':'planar-t044-source'}))
            (packet/'evidence/original-resolution.json').write_text('{}')
            class Result: stdout='[]'
            with patch('source_account._validate',return_value={}), patch('subprocess.run',return_value=Result()) as run, patch.dict(os.environ,{'UNRELATED_SECRET':'excluded'}):
                self.assertEqual(source_account.check(packet,root/'candidate',reviews)['source_account'],'resolved-and-clean')
                self.assertNotIn('UNRELATED_SECRET',run.call_args.kwargs['env'])
                self.assertEqual(run.call_args.kwargs['env']['PLANAR_EVALS_OUTPUT_ROOT'],str(scope.parent.resolve()))
                Result.stdout='["pending"]'
                with self.assertRaises(ValueError): source_account.check(packet,root/'candidate',reviews)
            (packet/'evidence/original-gate.json').write_text(json.dumps({'account_scope_id':'/outside/planar-account-admission-v1','execution_id':'planar-t044-source'}))
            with patch('source_account._validate',return_value={}), self.assertRaises((ValueError,FileNotFoundError)):
                source_account.check(packet,root/'candidate',reviews)
