import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import clean_retained_support


class HostSetup(unittest.IsolatedAsyncioTestCase):
    async def test_identity_rejected_before_source_or_guest(self):
        with TemporaryDirectory() as tmp:
            case=Path(tmp)/'case'; case.mkdir(); (case/'source-policy.json').write_text(json.dumps({'job':'source'}))
            with patch('clean_revision_support.load_case_module') as loader, self.assertRaises(ValueError):
                await clean_retained_support.prepare_target(None,case,case,case,case,'wrong','target','digest')
            loader.assert_not_called()

    async def test_host_setup_no_injection_and_reserves_target_time(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp); case=root/'case'; case.mkdir(); evidence=root/'evidence'; evidence.mkdir(); packet=root/'packet'; (packet/'source').mkdir(parents=True)
            (packet/'source-policy.json').write_text('{}'); (case/'source-policy.json').write_text(json.dumps({'job':'source'}))
            (evidence/'native-trial-start.json').write_text(json.dumps({'job_id':'target','job_dir':str(evidence/evidence.name)}))
            (packet/'source/pre-receipt.json').write_text(json.dumps({'wall_ns':1}))
            for name in ('materialize.py','install_fixture.py','projection.json','durable-policy.json'): (case/name).write_text('{}')
            (case/'fixture').mkdir()
            class Source:
                def validate(self,*args): return {'source_packet_sha256':'digest'}
            class Account:
                def check(self,*args): return {'source_account':'resolved-and-clean'}
            class Capture:
                def census(self,cid): return ['baseline']
                def load(self,path): return {'snapshot':b'bytes'}
                async def collect(self,*args,**kwargs): return {'snapshot':b'bytes'}
                def save(self,path,files): path.mkdir()
            class Boundary:
                async def handoff(self,*args,**kwargs): return {'boundary':'restored-no-fault'}
            class Platform:
                async def _resolve_service_container(self,name): return 'short'
            class Env:
                _platform=Platform()
                async def upload_dir(self,*args): pass
                async def exec(self,command,**kwargs):
                    class Result: return_code=0; stdout=json.dumps({'source_packet_sha256':'digest','status':'restored'})
                    return Result()
            class Process: stdout='container-id\n'
            modules={'source_packet':Source(),'source_account':Account(),'capture':Capture(),'no_fault':Boundary()}
            with patch('clean_revision_support.load_case_module',side_effect=lambda case,name:modules[name]), \
                 patch('clean_revision_support.check_revision_time') as clock, \
                 patch('retained_b_support._run',return_value={'source_packet_sha256':'digest'}), \
                 patch('subprocess.run',return_value=Process()):
                result=await clean_retained_support.prepare_target(Env(),case,packet,case,evidence,'source','target','digest')
            self.assertEqual(result['status'],'restored-no-fault'); clock.assert_called_once_with(evidence,565)
            receipt=json.loads((evidence/'retained-clean/no-fault.json').read_bytes())
            self.assertEqual((receipt['source_job'],receipt['target_job']),('source','target'))
            self.assertFalse((evidence/'retained-clean/injection.json').exists())

    async def test_separate_verifier_handoff_keeps_source_out_of_guest(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp); case=root/'case';case.mkdir(); packet=root/'packet';packet.mkdir(); candidate=root/'candidate';candidate.mkdir(); evidence=root/'evidence';evidence.mkdir(); destination=root/'export';destination.mkdir()
            (case/'candidate.json').write_text(json.dumps({'sha256':'candidate'})); (candidate/'package.md').write_text('package'); (packet/'host-only-source.txt').write_text('source evidence')
            target=evidence/'retained-clean'; (target/'logs/revision').mkdir(parents=True); (target/'container.json').write_text(json.dumps({'container_id':'id','baseline':['base']}))
            class Source:
                def validate(self,*args): return {'source_packet_sha256':'digest'}
            class Capture:
                async def collect(self,*args,**kwargs): return {'home/settings.yaml':b'settings'}
                def save(self,path,files): path.mkdir()
            class Platform:
                async def _resolve_service_container(self,name): return 'short'
            class Env: _platform=Platform()
            class Process: stdout='id\n'
            with patch('clean_revision_support.load_case_module',side_effect=lambda case,name:{'source_packet':Source(),'capture':Capture()}[name]), \
                 patch('candidate.inventory',return_value={'sha256':'candidate'}), patch('subprocess.run',return_value=Process()):
                result=await clean_retained_support.verifier_handoff(Env(),case,packet,candidate,evidence,'digest',destination)
            self.assertEqual(result['status'],'verifier-copy-ready')
            self.assertEqual((destination/'_planar_host/source-packet/host-only-source.txt').read_text(),'source evidence')
            self.assertFalse((destination/'settings.yaml').read_bytes()!=b'settings')
            self.assertFalse((destination/'host-only-source.txt').exists())
