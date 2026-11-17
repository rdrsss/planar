import copy
import json
import unittest
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'adapters'))
import launch
from harbor.models.job.config import JobConfig
from harbor.models.task.task import Task
import schema2_projection as p

class Projection(unittest.TestCase):
    def setUp(self):
        self.eid='a'*32; self.evidence='/tmp/job'; self.case='/tmp/case'
        self.job=JobConfig.model_validate(launch.config_for('planar-t044-'+self.eid,Path(self.evidence),Path(self.case),'medium'))
    def normalize(self, job=None): return p.normalize(job or self.job,execution_id=self.eid,evidence_raw=self.evidence,case_raw=self.case,effort='medium')
    def test_full_roundtrip_and_pointer_rules(self):
        full,policy=self.normalize()
        self.assertIsNone(full['agents'][0]['base_url'])
        self.assertEqual(full['agents'][0]['include_logs'],[])
        self.assertEqual(full['environment']['env'],{})
        self.assertEqual(policy['jobs_dir'],'<reserved-job-directory>')
        self.assertEqual(p.canonical(p.project(p.parse_closed(p.canonical(full),'medium',trusted=self.job))),p.canonical(full))
        for field in ('job_name','jobs_dir'):
            job=self.job.model_copy(deep=True); setattr(job,field,'wrong')
            with self.subTest(field=field), self.assertRaises(p.Invalid): self.normalize(job)
        job=self.job.model_copy(deep=True); job.tasks[0].path=Path('/other')
        with self.assertRaises(p.Invalid): self.normalize(job)
        job=self.job.model_copy(deep=True); job.agents[0].kwargs['reasoning_effort']='high'
        with self.assertRaises(p.Invalid): self.normalize(job)
        job=self.job.model_copy(deep=True); job.timeout_multiplier=2.0
        self.assertNotEqual(p.canonical(self.normalize(job)[1]),p.canonical(policy))
    def test_env_kwargs_and_secret_channels_refuse(self):
        for target in ('agent_env','environment_env','verifier_env','api_key_name','base_url','agent_extra','config_leaf','boolean_coercion','environment_extra','verifier_kwargs'):
            job=self.job.model_copy(deep=True)
            if target=='agent_env': job.agents[0].env={'SECRET':'not-persisted'}
            if target=='environment_env': job.environment.env={'SECRET':'not-persisted'}
            if target=='verifier_env': job.verifier.env={'SECRET':'not-persisted'}
            if target=='api_key_name': job.agents[0].model_api_key_name='SECRET'
            if target=='base_url': job.agents[0].base_url='https://example.invalid'
            if target=='agent_extra': job.agents[0].kwargs['secret']='not-persisted'
            if target=='config_leaf': job.agents[0].kwargs['config']['features']['apps']=True
            if target=='boolean_coercion': job.agents[0].kwargs['config']['features']['apps']=0
            if target=='environment_extra': job.environment.kwargs['x']=1
            if target=='verifier_kwargs': job.verifier.kwargs={'x':1}
            with self.subTest(target=target), self.assertRaises(p.Invalid): self.normalize(job)
    def test_each_public_config_leaf_is_closed(self):
        for section,key in [('plugins','_default'),('skills','bundled'),('features','apps'),('features','remote_plugin'),('features','multi_agent')]:
            job=self.job.model_copy(deep=True)
            target=job.agents[0].kwargs['config'][section]
            if section in ('plugins','skills'): target[key]['enabled']=True
            else: target[key]=not target[key]
            with self.subTest(section=section,key=key), self.assertRaises(p.Invalid): self.normalize(job)

    def test_verifier_copy_is_only_image_override(self):
        task=Task(Path(__file__).resolve().parents[1]/'cases/native-lineage-portable').config
        original=task.verifier.environment
        before=p.project(original)
        effective=p.effective_verifier_environment(original,'sha256:'+'b'*64)
        self.assertEqual(p.project(original),before)
        self.assertEqual(p.project(effective),{**before,'docker_image':'sha256:'+'b'*64})
        dirty=original.model_copy(deep=True); dirty.env={'TOKEN':'secret'}
        with self.assertRaises(p.Invalid): p.effective_verifier_environment(dirty,'sha256:'+'b'*64)
        dirty=original.model_copy(deep=True); dirty.docker_image='sha256:'+'a'*64
        with self.assertRaises(p.Invalid): p.effective_verifier_environment(dirty,'sha256:'+'b'*64)

    def test_closed_input_rejects_lost_and_coerced_fields(self):
        full=self.normalize()[0]
        for mutation in ('unknown','missing','coerced','duplicate'):
            value=copy.deepcopy(full)
            if mutation=='unknown': value['unknown']=1
            if mutation=='missing': del value['debug']
            if mutation=='coerced': value['n_attempts']='1'
            data=p.canonical(value)
            if mutation=='duplicate': data=data[:-2]+b',"debug":false}\n'
            with self.subTest(mutation=mutation), self.assertRaises(p.Invalid): p.parse_closed(data,'medium',trusted=self.job)

if __name__=='__main__': unittest.main()
