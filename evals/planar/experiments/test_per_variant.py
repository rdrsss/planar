import copy
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import per_variant as p

TEMPLATE = Path(__file__).resolve().parents[1] / 'cases/native-lineage-portable'
EXECUTION = {'schema_version':1, 'driver':{'model':'gpt-6.1-terra','effort':'high'}, 'child':{'model':'gpt-6.1-terra','effort':'medium'}}

class PortableInputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.candidate = self.root/'candidate'
        self.candidate.mkdir()
        (self.candidate/'file').write_bytes(b'candidate')
        self.pin = p.tree_hash(TEMPLATE)
        self.instance = self.root/'instance'
        self.ids = p.materialize(TEMPLATE, self.instance, self.candidate, EXECUTION, expected_template_sha256=self.pin)

    def check(self):
        return p.validate_instance(TEMPLATE, self.instance, self.candidate, expected_template_sha256=self.pin)

    def test_binding_and_immutable_template(self):
        self.assertEqual(self.ids, self.check())
        self.assertNotEqual(self.ids['case_template_sha256'], self.ids['case_instance_sha256'])
        with self.assertRaises(p.Invalid): p.materialize(TEMPLATE, self.instance, self.candidate, EXECUTION, expected_template_sha256=self.pin)
        (self.candidate/'extra').write_bytes(b'other')
        with self.assertRaises(p.Invalid): self.check()
        with self.assertRaises(TypeError): p.validate_instance(TEMPLATE, self.instance, expected_template_sha256=self.pin)

    def test_well_formed_wrong_inventory_refused(self):
        other = self.root/'other'; other.mkdir()
        (other/'unrelated').write_bytes(b'candidate')
        wrong = p.inventory(other)
        for slot in ('candidate.json', 'tests/bundle/manifest.json'):
            (self.instance/slot).write_bytes(wrong)
        with self.assertRaises(p.Invalid): self.check()

    def test_contract_and_reviewed_pin(self):
        self.assertIn('`planar/native-lineage-portable`, contract version 2', (TEMPLATE/'instruction.md').read_text())
        with self.assertRaises(p.Invalid): p.validate_template(TEMPLATE, '0'*64)
        for name, contents in [('candidate.json', b'bound\n'), ('portable-contract.json', p.canonical({**p.CONTRACT, 'contract_version':3})), ('portable-contract.json', p.canonical({**p.CONTRACT, 'slots':list(p.SLOTS)+['extra']}))]:
            altered = self.root/'template'
            shutil.copytree(TEMPLATE, altered)
            (altered/name).write_bytes(contents)
            with self.assertRaises(p.Invalid): p.validate_template(altered, p.tree_hash(altered))
            shutil.rmtree(altered)

    def test_slot_and_tree_swaps_refuse(self):
        for path in ('candidate.json', 'execution.json', 'tests/bundle/manifest.json', 'tests/bundle/execution.json'):
            slot = self.instance/path
            previous = slot.read_bytes()
            slot.write_bytes(b'{}\n')
            with self.assertRaises(p.Invalid): self.check()
            slot.write_bytes(previous)
        (self.instance/'instruction.md').write_text('changed')
        with self.assertRaises(p.Invalid): self.check()

    def test_tree_boundaries(self):
        extra = self.instance/'extra'; extra.write_text('extra')
        with self.assertRaises(p.Invalid): self.check()
        extra.unlink()
        target = self.instance/'instruction.md'; target.rename(self.instance/'renamed')
        with self.assertRaises(p.Invalid): self.check()
        (self.instance/'renamed').rename(target)
        target.unlink(); target.mkdir()
        with self.assertRaises(p.Invalid): self.check()
        target.rmdir(); target.symlink_to(TEMPLATE/'instruction.md')
        with self.assertRaises(p.Invalid): self.check()

    def test_canonical_and_closed_inventory(self):
        data = p.inventory(self.candidate); obj = p.validate_inventory(data)
        for bad in (data.rstrip(), data.replace(b'"algorithm"', b'"algorithm" '), p.canonical({**obj, 'extra':1}), p.canonical({**obj, 'sha256':'0'*64}), b' '* (1024*1024+1)):
            with self.assertRaises(p.Invalid): p.validate_inventory(bad)
        for paths in (['../file'], ['/file'], ['a//b'], ['a\\b'], ['x','x'], ['z','a']):
            changed = copy.deepcopy(obj)
            changed['files'] = [{'path':path,'sha256':'a'*64} for path in paths]
            changed['sha256'] = p.digest(p.canonical(changed['files'])[:-1])
            with self.assertRaises(p.Invalid): p.validate_inventory(p.canonical(changed))

    def test_symlink_and_effort_refuse(self):
        (self.candidate/'link').symlink_to(self.candidate/'file')
        with self.assertRaises(p.Invalid): p.inventory(self.candidate)
        for key in ('driver','child'):
            bad = copy.deepcopy(EXECUTION); bad[key]['effort'] = 'unsupported'
            with self.assertRaises(p.Invalid): p.validate_execution(p.canonical(bad))
        with self.assertRaises(p.Invalid): p.validate_execution(b' ' * 4097)

class PortableVerifier(PortableInputs):
    def setUp(self):
        super().setUp()
        spec = importlib.util.spec_from_file_location('portable_verifier', TEMPLATE/'tests/verify.py')
        self.verifier = importlib.util.module_from_spec(spec)
        source = (TEMPLATE/'tests/verify.py').read_bytes()
        exec(compile(source, str(TEMPLATE/'tests/verify.py'), 'exec'), self.verifier.__dict__)
        self.bundle = self.instance/'tests/bundle'

    def test_bundle_is_validated(self):
        self.verifier.manifest_check(self.bundle)
        (self.bundle/'execution.json').write_bytes(b'{}\n')
        with self.assertRaises(ValueError): self.verifier.manifest_check(self.bundle)

    def test_exact_result_and_missing_evidence(self):
        workspace = self.root/'workspace'; workspace.mkdir()
        agent = self.root/'agent'; (agent/'sessions').mkdir(parents=True)
        for name in ('rollout-a.jsonl','rollout-b.jsonl'): (agent/'sessions'/name).write_text('x')
        rows = [[{'type':'session_meta','payload':{'id':'parent','source':'exec'}}, {'type':'event_msg','payload':{'type':'task_complete'}}], [{'type':'session_meta','payload':{'id':'child','source':'child'}}, {'type':'event_msg','payload':{'type':'task_complete'}}]]
        with patch.object(self.verifier, 'events', side_effect=rows), patch.object(self.verifier, 'grade', return_value={}):
            (workspace/'RESULT.txt').write_bytes(b'2\nextra')
            self.assertEqual(self.verifier.assess(workspace, agent, self.bundle)['status'], 'behavioral_failure')
        with patch.object(self.verifier, 'events', side_effect=rows), patch.object(self.verifier, 'grade', return_value={}):
            (workspace/'RESULT.txt').write_bytes(b'2\n')
            self.assertEqual(self.verifier.assess(workspace, agent, self.bundle)['status'], 'pass')

    def test_thread_setting_contradiction(self):
        v = self.verifier; v.manifest_check(self.bundle)
        def event(t, typ, payload): return {'timestamp':f'2026-01-01T00:00:{t:02d}+00:00','type':typ,'payload':payload}
        args = {'fork_turns':'none','model':'gpt-6.1-terra','reasoning_effort':'medium','task_name':'worker'}
        parent = [event(0,'session_meta',{'id':'p','source':'exec','originator':'codex_exec'}), event(1,'turn_context',{'model':'gpt-6.1-terra','effort':'medium'}), event(2,'response_item',{'type':'function_call','namespace':'collaboration','name':'spawn_agent','call_id':'s','arguments':json.dumps(args)}), event(4,'response_item',{'type':'function_call_output','call_id':'s','output':json.dumps({'task_name':'/root/worker'})}), event(6,'event_msg',{'type':'task_complete'})]
        child = [event(3,'session_meta',{'id':'c','parent_thread_id':'p','agent_path':'/root/worker','source':{'subagent':{'thread_spawn':{'parent_thread_id':'p','depth':1,'agent_path':'/root/worker'}}}}), event(5,'event_msg',{'type':'task_complete'})]
        self.assertEqual(v.grade(parent,child)['status'],'behavioral_failure')
        parent[1]['payload']['effort'] = 'high'
        # Correct parent effort passes the settings check and then reaches the missing inventory guard.
        with self.assertRaises(v.BehaviorError): v.grade(parent,child)

if __name__ == '__main__': unittest.main()
