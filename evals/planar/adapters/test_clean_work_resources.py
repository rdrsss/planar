import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from clean_work_resources import check_projects


class CleanWorkResources(TestCase):
    def test_project_coverage_and_failed_queries(self):
        with TemporaryDirectory() as temp:
            evidence=Path(temp)/('planar-t044-'+'a'*32); registry=evidence/'projects'; registry.mkdir(parents=True)
            prefix=(evidence.name+'-case__xyz').lower(); env=prefix+'__env'; worker=prefix+'__verifier__worker'
            for name in (env, worker):
                (registry/(hashlib.sha256(name.encode()).hexdigest()+'.json')).write_text(json.dumps({'execution_id':evidence.name,'compose_project':name}))
            (evidence/'verifier-compose-up-worker.json').write_text(json.dumps({'execution_id':evidence.name,'command':['up','--detach','--wait','--no-build','--pull','never']}))
            trial={'step_results':[{'step_name':'worker','verifier':{'started_at':'2026-01-01'}},{'step_name':'review','verifier':None}]}
            calls=[]
            def empty(cmd,**kwargs): calls.append(cmd); return SimpleNamespace(returncode=0,stdout='')
            result=check_projects(evidence,'case__XYZ',trial,run=empty)
            self.assertEqual(len(result['queried_projects']),3); self.assertEqual(len(calls),9)
            with self.assertRaisesRegex(ValueError,'absence unproven'): check_projects(evidence,'case__XYZ',trial,run=lambda *a,**k: SimpleNamespace(returncode=1,stdout=''))
            with self.assertRaisesRegex(ValueError,'absence unproven'): check_projects(evidence,'case__XYZ',trial,run=lambda *a,**k: SimpleNamespace(returncode=0,stdout='live'))
            extra=evidence/'verifier-compose-up-review.json'; extra.write_text(json.dumps({'execution_id':evidence.name,'command':['up','--detach','--wait','--no-build','--pull','never']}))
            with self.assertRaisesRegex(ValueError,'lacks registry'): check_projects(evidence,'case__XYZ',trial,run=empty)
            extra.unlink()
            receipt=evidence/'verifier-compose-up-worker.json'; original=receipt.read_text(); receipt.write_text('{}')
            with self.assertRaisesRegex(ValueError,'receipt mismatch'): check_projects(evidence,'case__XYZ',trial,run=empty)
            receipt.write_text(original)
            (registry/(hashlib.sha256(worker.encode()).hexdigest()+'.json')).unlink()
            with self.assertRaisesRegex(ValueError,'lacks registry'): check_projects(evidence,'case__XYZ',trial,run=empty)
            with self.assertRaisesRegex(ValueError,'coverage unavailable'): check_projects(evidence,'case__XYZ',{'step_results':trial['step_results'][:1]},run=empty)
            with self.assertRaisesRegex(ValueError,'coverage ambiguous'): check_projects(evidence,'case__XYZ',{'step_results':[trial['step_results'][0]]*2},run=empty)
