import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
import per_variant
import schema2
import schema2_admission as admission

TEMPLATE = Path(__file__).resolve().parents[1]/'cases/native-lineage-portable'
def when(seconds): return (datetime.now(timezone.utc)+timedelta(seconds=seconds)).isoformat()

class AdmissionHandoff(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name); self.root=self.base/'experiment'; self.reviews=self.base/'reviews'; self.reviews.mkdir()
        source=self.base/'candidate'; (source/'.claude-plugin').mkdir(parents=True)
        (source/'.claude-plugin/plugin.json').write_text(json.dumps({'name':'planar','version':'0.1.0'}))
        for name in schema2.SKILLS:
            d=source/'skills'/name; d.mkdir(parents=True); (d/'SKILL.md').write_text(name)
        variant={'id':'a','candidate_source':str(source),'effort':'medium','image_id':'sha256:'+'a'*64,'verifier_image_id':'sha256:'+'b'*64,'host_version':'test'}
        config={'model':'gpt-6.1-terra','variant_efforts':{'a':'medium'},'native_config':{'fixture':'synthetic'},'caps':{'session_seconds':5,'verifier_seconds':5,'trial_seconds':5,'job_seconds':3600,'trials':1,'max_executions':1}}
        m=schema2.initialize(self.root,template=TEMPLATE,reviewed_template_sha256=per_variant.tree_hash(TEMPLATE),variants=[variant],repetitions=1,config=config,implementation_sha256='c'*64,deadline_at=when(3600),account_scope_id='scope')
        self.identity=m['identity']; self.r=schema2.reserve(self.root,self.identity,'a:1',expires_at=when(600))
        self.gate={k:self.r[k] for k in schema2.GATE_FIELDS if k in self.r}
        self.gate.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':schema2.h((self.root/'reservations'/f"{self.r['reservation_id']}.json").read_bytes()),'reviewer':'independent','report':'independent.md','reviewed_at':when(-1)})
        self.path=self.reviews/'gate.json'; self.path.write_text(json.dumps(self.gate,indent=2))

    def prepare(self): return admission.prepare(self.root,self.identity,self.r['reservation_id'],self.path,reviews_root=self.reviews,account_scope_id='scope')

    def test_read_only_handoff_matches_existing_marker_algorithm(self):
        result=self.prepare()
        expected=hashlib.sha256(json.dumps(self.gate,sort_keys=True,separators=(',', ':')).encode()).hexdigest()
        self.assertEqual(result['gate_marker_sha256'],expected)
        self.assertEqual(result['execution_id'],self.r['reservation_id'])
        self.assertEqual(result['authorization'],'not granted')
        self.assertFalse(result['gate_consumed']); self.assertFalse(result['gate_marker_exists'])
        self.assertEqual(list((self.root/'executions').iterdir()),[])
        self.assertEqual(list(self.reviews.iterdir()),[self.path])
        consumed=self.reviews/'harbor-t044-consumed'; consumed.mkdir(); (consumed/expected).write_text('reserved\n')
        self.assertTrue(self.prepare()['gate_marker_exists'])

    def test_refuses_scope_gate_and_input_swaps(self):
        with self.assertRaises(schema2.Invalid): admission.prepare(self.root,self.identity,self.r['reservation_id'],self.path,reviews_root=self.reviews,account_scope_id='wrong')
        altered=copy.deepcopy(self.gate); altered['verifier_image_id']='sha256:'+'e'*64
        self.path.write_text(json.dumps(altered))
        with self.assertRaises(schema2.Invalid): self.prepare()
        self.path.write_text(json.dumps(self.gate))
        (self.root/'variants/a/candidate/skills/run/SKILL.md').write_text('drift')
        with self.assertRaises((schema2.Invalid,per_variant.Invalid)): self.prepare()

    def test_rejects_unsafe_or_ambiguous_review(self):
        raw=json.dumps(self.gate); self.path.write_text(raw[:-1]+',"verdict":"approve"}')
        with self.assertRaises(schema2.Invalid): self.prepare()
        self.path.write_text(raw)
        alias=self.reviews/'alias'; alias.symlink_to(self.path)
        with self.assertRaises(schema2.Invalid): admission.prepare(self.root,self.identity,self.r['reservation_id'],alias,reviews_root=self.reviews,account_scope_id='scope')
        outside=self.base/'gate.json'; outside.write_text(raw)
        with self.assertRaises(schema2.Invalid): admission.prepare(self.root,self.identity,self.r['reservation_id'],outside,reviews_root=self.reviews,account_scope_id='scope')
        (self.reviews/'planar-account-admission-v1').symlink_to(self.base,target_is_directory=True)
        with self.assertRaises(schema2.Invalid): self.prepare()

if __name__=='__main__': unittest.main()
