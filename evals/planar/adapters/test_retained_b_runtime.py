import hashlib
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch
import launch
from retained_b_support import verify_runtime

def h(value): return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()

class RuntimeAuthority(TestCase):
    def test_shared_gate_swap_rejected_even_when_local_gate_unchanged(self):
        with TemporaryDirectory() as temp, patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':temp,'PLANAR_T045_VERIFIER_IMAGE_ID':'v','PLANAR_T044_EXPECTED_IMAGE_ID':'i','PLANAR_T044_ABSOLUTE_DEADLINE':'d'},clear=True):
            root=Path(temp).resolve(); eid='planar-t044-'+'a'*32; evidence=root/'jobs'/eid; evidence.mkdir(parents=True); scope=root/'planar-account-admission-v1'; records=scope/'records'; records.mkdir(parents=True)
            packet=root/'packet'; packet.mkdir(); case=root/'case'; case.mkdir(); candidate=root/'candidate'; candidate.mkdir(); config={'job':'fixed'}
            gate={'execution_id':eid,'case':'retained-a-fresh-b','source_packet_sha256':'s','source_packet_root':str(packet),'case_sha256':'c','candidate_sha256':'p','effective_configuration_sha256':h(config),'verifier_image_id':'v','image_id':'i','absolute_deadline':'d','verdict':'approve','account_scope_id':str(scope),'implementation_sha256':'impl','configuration_sha256':'config'}
            gatepath=root/'gate.json'; gatepath.write_text(json.dumps(gate)); gatehash=hashlib.sha256(gatepath.read_bytes()).hexdigest(); consumed=root/'harbor-t044-consumed'; consumed.mkdir(); (consumed/h(gate)).write_text('reserved\n')
            record={'execution_id':eid,'gate_path':str(gatepath),'gate_raw_sha256':gatehash,'gate_authority':gate,'source_packet_sha256':'s','source_packet_root':str(packet),'account_scope_id':str(scope),'case_sha256':'c','candidate_sha256':'p','effective_configuration_sha256':h(config),'verifier_image_id':'v'}
            (evidence/'retained-admission.json').write_text(json.dumps(record)); (evidence/'job-config.json').write_text(json.dumps(config)); (evidence/'launcher.json').write_text(json.dumps({**{k:gate[k] for k in ('execution_id','case','case_sha256','candidate_sha256','implementation_sha256','configuration_sha256','image_id')},'review_sha256':gatehash}))
            shared={'execution_id':eid,'case':'retained-a-fresh-b','gate_sha256':gatehash,'gate_authority':gate,'status':'awaiting-independent-bound-raw-audit'}; path=records/(eid+'.start.json'); path.write_text(json.dumps(shared))
            with patch.object(launch,'case_inventory',return_value='c'),patch.object(launch,'source_digest',return_value='impl'),patch('candidate.inventory',return_value={'sha256':'p'}),patch('retained_b_support.source',return_value={'source_packet_sha256':'s'}):
                verify_runtime(evidence,case,packet,candidate)
                path.write_text(json.dumps({**shared,'gate_sha256':'wrong'}))
                with self.assertRaisesRegex(ValueError,'shared authority'): verify_runtime(evidence,case,packet,candidate)
