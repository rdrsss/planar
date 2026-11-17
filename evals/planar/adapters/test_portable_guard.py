import asyncio
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch, AsyncMock
from types import SimpleNamespace
from harbor.environments.docker.docker import DockerEnvironment
from harbor.models.task.config import NetworkMode, NetworkPolicy
from harbor.models.task.task import Task
from harbor.models.trial.paths import TrialPaths
from guarded_docker import GuardedDocker
import launch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'experiments'))
import schema2_projection as projection

CASE = Path(__file__).resolve().parents[1]/'cases/native-lineage-portable'

class PortableGuard(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name).resolve(); self.case=self.root/'case'; shutil.copytree(CASE,self.case)
        self.evidence=self.root/('planar-t044-noauth-'+'a'*32); self.evidence.mkdir()
        self.original=Task(self.case).config.verifier.environment
        self.image='sha256:'+'b'*64
        digest=launch.case_inventory(self.case)
        self.record={'execution_id':self.evidence.name,'case':'native-lineage-portable','case_sha256':digest,'prerequisites':{'case_read_root':str(self.case)},'runnable_contract':'planar-schema2-runnable-v1','verifier_image_id':self.image,'effective_verifier_environment':projection.project(projection.effective_verifier_environment(self.original,self.image))}
        (self.evidence/'launcher.json').write_text(json.dumps(self.record))
        env={'PLANAR_T044_EVIDENCE_DIR':str(self.evidence),'PLANAR_T044_CASE':'native-lineage-portable','PLANAR_T044_CASE_SNAPSHOT':str(self.case),'PLANAR_T044_CANDIDATE_SNAPSHOT':str(self.root/'candidate'),'PLANAR_T044_CASE_SHA256':digest,'PLANAR_T045_VERIFIER_IMAGE_ID':self.image}
        self.patch=patch.dict(os.environ,env,clear=True); self.patch.start(); self.addCleanup(self.patch.stop)
    def construct(self):
        return GuardedDocker(environment_dir=self.case/'tests',environment_name='verifier',session_id='trial__verifier__',trial_paths=TrialPaths(self.evidence/'trial'),task_env_config=self.original,network_policy=NetworkPolicy(network_mode=NetworkMode.NO_NETWORK),auto_mounting=False)
    def test_only_deep_copy_changes_image(self):
        before=projection.project(self.original); env=self.construct()
        self.assertEqual(projection.project(self.original),before)
        self.assertIsNone(self.original.docker_image)
        self.assertEqual(env.task_env_config.docker_image,self.image)
        self.assertEqual(projection.project(env.task_env_config),self.record['effective_verifier_environment'])
    def test_wrong_binding_fails_before_environment_creation(self):
        self.record['verifier_image_id']='sha256:'+'c'*64
        (self.evidence/'launcher.json').write_text(json.dumps(self.record))
        with self.assertRaises(ValueError): self.construct()
    def test_actual_image_and_network_observations_are_required(self):
        env=self.construct()
        async def start_parent(instance,force_build): return None
        def inspect(command,**kwargs):
            return SimpleNamespace(stdout=('none' if command[3]=='{{.HostConfig.NetworkMode}}' else self.image)+'\n')
        async def exercise():
            with patch.object(DockerEnvironment,'start',start_parent), patch.object(env._platform,'_resolve_service_container',AsyncMock(return_value='container')), patch('guarded_docker.subprocess.run',side_effect=inspect):
                await env.start(False)
        asyncio.run(exercise())
        observed=json.loads((self.evidence/'verifier-network.json').read_text())
        self.assertEqual(observed['image_id'],self.image)
        self.assertEqual(observed['network_mode'],'none')
        for bad_network,bad_image in [('bridge',self.image),('none','sha256:'+'c'*64)]:
            def bad_inspect(command,**kwargs):
                return SimpleNamespace(stdout=(bad_network if command[3]=='{{.HostConfig.NetworkMode}}' else bad_image)+'\n')
            async def negative():
                with patch.object(DockerEnvironment,'start',start_parent), patch.object(env._platform,'_resolve_service_container',AsyncMock(return_value='container')), patch('guarded_docker.subprocess.run',side_effect=bad_inspect):
                    with self.assertRaises((ValueError,RuntimeError)): await env.start(False)
            asyncio.run(negative())

    def test_compose_up_forbids_pull_and_build(self):
        env=self.construct(); calls=[]
        async def fake(instance, command, **kwargs): calls.append(command); return 'stub'
        async def exercise():
            with patch.object(DockerEnvironment,'_run_docker_compose_command',fake):
                self.assertEqual(await env._run_docker_compose_command(['up','--detach']),'stub')
                for command in (['build'],['pull'],['up','--build'],['up','--pull=always']):
                    with self.assertRaises(ValueError): await env._run_docker_compose_command(command)
                with self.assertRaises(ValueError): await env.start(True)
        asyncio.run(exercise())
        self.assertEqual(calls,[['up','--detach','--no-build','--pull','never']])

if __name__=='__main__': unittest.main()
