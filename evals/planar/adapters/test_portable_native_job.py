import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import AsyncMock, Mock, patch
from harbor.models.job.config import JobConfig
import launch
import native_job
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'experiments'))
import schema2_execution
import schema2_projection

class PortableNativeJob(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.eid='a'*32; self.evidence=Path(self.tmp.name).resolve()/('planar-t044-'+self.eid); self.evidence.mkdir()
        self.case=Path(self.tmp.name).resolve()/'case'
        self.config=JobConfig.model_validate(launch.config_for(self.evidence.name,self.evidence,self.case,'medium'))
        self.path=self.evidence/'config.json'; self.path.write_bytes(schema2_projection.canonical(schema2_projection.project(self.config)))
        self.context=self.evidence/'schema2-context.json'; self.context.write_text(json.dumps({'root':'scratch','identity':'id','reservation_id':self.eid,'preparation_evidence':'prep','gate_path':'gate'}))
        self.env=patch.dict(os.environ,{'PLANAR_SCHEMA2_CONTEXT':str(self.context)},clear=False); self.env.start(); self.addCleanup(self.env.stop)
    async def test_exact_config_reaches_job_create(self):
        binding={'reservation':{'concrete_binding':{'job_config':schema2_projection.project(self.config)}}}
        job=Mock(); job.on_trial_started=Mock(); job.run=AsyncMock()
        with patch.object(schema2_execution,'verify_activated',return_value=binding), patch.object(native_job.Job,'create',new=AsyncMock(return_value=job)) as create:
            await native_job.run(self.path,self.evidence)
            self.assertEqual(create.await_count,1); self.assertEqual(job.run.await_count,1)
    async def test_raw_mutations_refuse_before_job_create(self):
        expected=schema2_projection.project(self.config)
        binding={'reservation':{'concrete_binding':{'job_config':expected}}}
        for mutation in ('unknown','coerced','duplicate','noncanonical'):
            value=json.loads(schema2_projection.canonical(expected))
            if mutation=='unknown': value['unreviewed']='secret'
            if mutation=='coerced': value['n_attempts']='1'
            raw=schema2_projection.canonical(value)
            if mutation=='duplicate': raw=raw[:-2]+b',"debug":false}\n'
            if mutation=='noncanonical': raw=json.dumps(value,indent=2).encode()
            self.path.write_bytes(raw)
            with self.subTest(mutation=mutation), patch.object(schema2_execution,'verify_activated',return_value=binding), patch.object(native_job.Job,'create',new=AsyncMock()) as create:
                with self.assertRaises(ValueError): await native_job.run(self.path,self.evidence)
                self.assertEqual(create.await_count,0)

    async def test_changed_default_refuses_before_job_create(self):
        expected=schema2_projection.project(self.config); expected['timeout_multiplier']=2.0
        binding={'reservation':{'concrete_binding':{'job_config':expected}}}
        with patch.object(schema2_execution,'verify_activated',return_value=binding), patch.object(native_job.Job,'create',new=AsyncMock()) as create:
            with self.assertRaises(ValueError): await native_job.run(self.path,self.evidence)
            self.assertEqual(create.await_count,0)

if __name__=='__main__': unittest.main()
