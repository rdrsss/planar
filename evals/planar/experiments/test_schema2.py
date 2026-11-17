import copy
from datetime import datetime, timedelta, timezone
import json
import shutil
from pathlib import Path
import tempfile
import unittest

import per_variant as p
import schema2 as s

TEMPLATE = Path(__file__).resolve().parents[1]/'cases/native-lineage-portable'

def timestamp(delta): return (datetime.now(timezone.utc)+timedelta(seconds=delta)).isoformat()

class SchemaTwo(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name); self.root = self.base/'experiment'
        variants = []
        for name, effort in [('a','medium'),('b','high')]:
            root = self.base/name; (root/'.claude-plugin').mkdir(parents=True)
            (root/'.claude-plugin/plugin.json').write_text(json.dumps({'name':'planar','version':'0.1.0'}))
            for skill in s.SKILLS:
                path = root/'skills'/skill; path.mkdir(parents=True); (path/'SKILL.md').write_text(name)
            variants.append({'id':name,'candidate_source':str(root),'effort':effort,'image_id':'sha256:'+'a'*64,'verifier_image_id':'sha256:' + ('b' if name == 'a' else 'c')*64,'host_version':'codex test'})
        self.manifest = s.initialize(self.root, template=TEMPLATE, reviewed_template_sha256=p.tree_hash(TEMPLATE), variants=variants, repetitions=2, config={'model':'gpt-6.1-terra','variant_efforts':{'a':'medium','b':'high'},'native_config':{'fixture':'synthetic'},'caps':{'session_seconds':5,'verifier_seconds':5,'trial_seconds':5,'job_seconds':3600,'trials':4,'max_executions':6}}, implementation_sha256='d'*64, deadline_at=timestamp(3600), account_scope_id='scratch')
        self.identity = self.manifest['identity']

    def reserve(self): return s.reserve(self.root,self.identity,'a:1',expires_at=timestamp(600))
    def gate(self, r):
        g = {k:r[k] for k in s.GATE_FIELDS if k in r}
        g.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':s.h((self.root/'reservations'/f"{r['reservation_id']}.json").read_bytes()), 'reviewer':'independent','report':'review.md','reviewed_at':timestamp(-1)})
        return g

    def test_two_by_two_and_variant_selection(self):
        self.assertEqual([t['logical_id'] for t in self.manifest['trials']], ['a:1','b:1','a:2','b:2'])
        a = s.preflight(self.root,self.identity,'a:1'); b = s.preflight(self.root,self.identity,'b:1')
        self.assertNotEqual(a['variant']['candidate_inventory_sha256'], b['variant']['candidate_inventory_sha256'])
        self.assertEqual(a['variant']['case_template_sha256'], b['variant']['case_template_sha256'])
        self.assertEqual(a['variant']['verifier_code_sha256'], b['variant']['verifier_code_sha256'])
        self.assertNotEqual(a['variant']['trusted_inputs_sha256'], b['variant']['trusted_inputs_sha256'])
        self.assertEqual(p.validate_execution((Path(b['instance'])/'execution.json').read_bytes())['driver']['effort'], 'high')

    def test_configuration_hash_covers_selected_execution(self):
        variant = self.manifest['variants'][0]
        config = variant['configuration']
        self.assertEqual(variant['configuration_sha256'], s.object_hash(config))
        for field, value in [('task', {'case':'different','contract_version':2}), ('image_id','sha256:'+'f'*64), ('verifier_image_id','sha256:'+'f'*64), ('host_version','different')]:
            changed = copy.deepcopy(config); changed[field] = value
            self.assertNotEqual(variant['configuration_sha256'], s.object_hash(changed))

    def test_continuation_uses_snapshot_after_source_deletion(self):
        shutil.rmtree(self.base/'a')
        selected = s.preflight(self.root,self.identity,'a:1')
        self.assertEqual(Path(selected['candidate']), self.root/'variants/a/candidate')
        self.assertTrue(Path(selected['candidate']).is_dir())

    def test_root_control_and_lock_symlinks_refuse(self):
        alias = self.base/'alias'; alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(s.Invalid): s.preflight(alias,self.identity,'a:1')
        lock = self.root/'reservation.lock'; target = self.base/'unrelated'; target.write_text('unchanged')
        lock.symlink_to(target)
        with self.assertRaises(s.Invalid): self.reserve()
        self.assertEqual(target.read_text(), 'unchanged')
        lock.unlink()
        real = self.root/'variants'; real.rename(self.root/'hidden-variants'); real.symlink_to(self.root/'hidden-variants', target_is_directory=True)
        with self.assertRaises(s.Invalid): s.preflight(self.root,self.identity,'a:1')

    def test_reservation_gate_and_no_activation(self):
        r = self.reserve(); gate = self.gate(r)
        self.assertEqual(s.validate_gate(self.root,self.identity,r['reservation_id'],gate)['trial']['logical_id'],'a:1')
        self.assertFalse((self.root/'executions'/r['execution_id']).exists())
        self.assertEqual(list((self.root/'executions').iterdir()), [])
        with self.assertRaises(s.Invalid): self.reserve()
        for field, value in [('execution_id','other'),('image_id','sha256:'+'f'*64),('verifier_image_id','sha256:'+'f'*64),('variant_id','b'),('account_scope_id','other'),('configuration_sha256','0'*64),('expires_at',timestamp(7200))]:
            wrong = dict(gate); wrong[field] = value
            with self.subTest(field=field), self.assertRaises(s.Invalid): s.validate_gate(self.root,self.identity,r['reservation_id'],wrong)
        wrong = dict(gate, extra=True)
        with self.assertRaises(s.Invalid): s.validate_gate(self.root,self.identity,r['reservation_id'],wrong)
        (self.root/'executions'/r['execution_id']).mkdir()
        with self.assertRaises(s.Invalid): s.validate_gate(self.root,self.identity,r['reservation_id'],gate)

    def test_swap_and_drift_refuse_preflight(self):
        r = self.reserve(); gate = self.gate(r)
        (self.root/'variants/a/candidate/skills/run/SKILL.md').write_text('drift')
        with self.assertRaises((s.Invalid,p.Invalid)): s.validate_gate(self.root,self.identity,r['reservation_id'],gate)
        self.assertFalse((self.root/'executions'/r['execution_id']).exists())

    def test_retry_requires_latest_incomplete(self):
        r = self.reserve()
        folder = self.root/'executions'/r['execution_id']; folder.mkdir()
        s.write_new(folder/'terminal.json', {'status':'incomplete'})
        retry = s.reserve(self.root,self.identity,'a:1',expires_at=timestamp(600),predecessor_execution_id=r['execution_id'],retry_reason='interrupted')
        self.assertEqual(retry['predecessor_execution_id'],r['execution_id'])
        with self.assertRaises(s.Invalid): s.reserve(self.root,self.identity,'a:1',expires_at=timestamp(600),predecessor_execution_id=r['execution_id'],retry_reason='again')
        self.assertEqual(s.validate_gate(self.root,self.identity,retry['reservation_id'],self.gate(retry))['trial']['logical_id'],'a:1')

if __name__ == '__main__': unittest.main()
