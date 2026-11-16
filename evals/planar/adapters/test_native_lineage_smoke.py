import copy
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from test_regrade_dispatch import fixture
import launch

CASE=Path(__file__).resolve().parents[1]/'cases/native-lineage-smoke'
spec=importlib.util.spec_from_file_location('lineage_verify_test',CASE/'tests/verify.py')
verify=importlib.util.module_from_spec(spec);spec.loader.exec_module(verify)

class LineageControls(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.workspace=self.root/'workspace';self.workspace.mkdir();(self.workspace/'RESULT.txt').write_bytes(b'2\n')
        self.sessions=self.root/'agent/sessions';self.sessions.mkdir(parents=True)
        self.parent,self.child,_=fixture();self.parent.append({'type':'event_msg','timestamp':'2026-11-16T12:01:00+00:00','payload':{'type':'task_complete'}})
    def write(self):
        for name,events in (('parent',self.parent),('child',self.child)):
            (self.sessions/('rollout-'+name+'.jsonl')).write_text(''.join(json.dumps(e)+'\n' for e in events))
    def assess(self):
        self.write();return verify.assess(self.workspace,self.root/'agent')
    def test_positive_does_not_require_commands_or_claim_causality(self):
        self.parent=[e for e in self.parent if not str(e['payload'].get('call_id','')).startswith('r')]
        self.child=[e for e in self.child if e['payload'].get('call_id')!='write']
        result=self.assess();self.assertEqual(result['status'],'pass')
        for key in ('resource_reads','read_before_spawn','child_write_causality','role_instruction_delivery','backend_identity'):
            self.assertEqual(result['dimensions'][key],'unverified')
    def test_wrong_bytes_symlink_and_host_inventory(self):
        result=self.workspace/'RESULT.txt';result.write_bytes(b'2');self.assertEqual(self.assess()['status'],'behavioral_failure')
        result.unlink();result.symlink_to('/etc/passwd');self.assertEqual(self.assess()['status'],'behavioral_failure')
        result.unlink();result.write_bytes(b'2\n');self.parent=[e for e in self.parent if e['type']!='world_state']
        self.assertEqual(self.assess()['status'],'behavioral_failure')
    def test_wrong_pins_freshness_lineage_and_order(self):
        for mutation in ('model','effort','freshness','lineage','order'):
            p,c=copy.deepcopy(self.parent),copy.deepcopy(self.child)
            spawn=next(e for e in p if e['payload'].get('name')=='spawn_agent')
            if mutation in ('model','effort','freshness'):
                args=json.loads(spawn['payload']['arguments']);args[{'model':'model','effort':'reasoning_effort','freshness':'fork_turns'}[mutation]]='wrong';spawn['payload']['arguments']=json.dumps(args)
            elif mutation=='lineage':c[0]['payload']['parent_thread_id']='wrong'
            else:c[0]['timestamp']='2020-01-01T00:00:00+00:00'
            oldp,oldc=self.parent,self.child;self.parent,self.child=p,c
            with self.subTest(mutation=mutation):self.assertEqual(self.assess()['status'],'behavioral_failure')
            self.parent,self.child=oldp,oldc
    def test_absent_evidence_incomplete_and_candidate_mismatch(self):
        self.write();(self.sessions/'rollout-child.jsonl').unlink()
        with self.assertRaises(verify.EvidenceError):verify.assess(self.workspace,self.root/'agent')
        self.child=self.child[:-1];self.assertEqual(self.assess()['status'],'incomplete')
        bundle=self.root/'bundle';shutil.copytree(verify.BUNDLE,bundle)
        data=json.loads((bundle/'manifest.json').read_text());data['files'][0]['sha256']='0'*64;(bundle/'manifest.json').write_text(json.dumps(data))
        with self.assertRaises(verify.EvidenceError):verify.assess(self.workspace,self.root/'agent',bundle)
        with patch.object(launch,'inventory',return_value={}):
            with self.assertRaisesRegex(ValueError,'candidate'):launch.plan('native-lineage-smoke')
    def test_inventory_is_unique_initial_native_message(self):
        original=copy.deepcopy(self.parent)
        for variant in ('late','duplicate','root','skill','agent_only'):
            self.parent=copy.deepcopy(original)
            inv=next(e for e in self.parent if e['type']=='world_state')
            if variant=='late': self.parent.remove(inv);self.parent.append(inv)
            elif variant=='duplicate': self.parent.append(copy.deepcopy(inv))
            elif variant=='root': inv['payload']['state']['host_skills']['body']=inv['payload']['state']['host_skills']['body'].replace('/installed-agent/','/wrong/')
            elif variant=='skill': inv['payload']['state']['host_skills']['body']=inv['payload']['state']['host_skills']['body'].replace('planar:run:', 'planar:gone:')
            else: inv['type']='response_item';inv['payload']={'type':'message','role':'assistant','content':inv['payload']['state']['host_skills']['body']}
            with self.subTest(variant=variant):
                if variant=='duplicate':
                    with self.assertRaisesRegex(verify.EvidenceError,'ambiguous host'):self.assess()
                else:self.assertEqual(self.assess()['status'],'behavioral_failure')
        self.parent=original

    def test_parent_child_completion_and_spawn_return_shape(self):
        originalp,originalc=copy.deepcopy(self.parent),copy.deepcopy(self.child)
        for variant in ('parent_duplicate','parent_early','child_duplicate','child_early','return_kind','return_absent','return_duplicate','return_wrong'):
            self.parent,self.child=copy.deepcopy(originalp),copy.deepcopy(originalc)
            parent_done=next(e for e in self.parent if e['payload'].get('type')=='task_complete')
            child_done=next(e for e in self.child if e['payload'].get('type')=='task_complete')
            ret=next(e for e in self.parent if e['payload'].get('call_id')=='spawn' and e['payload'].get('type')=='function_call_output')
            if variant=='parent_duplicate':self.parent.append(copy.deepcopy(parent_done))
            elif variant=='parent_early':self.parent.remove(parent_done);self.parent.insert(self.parent.index(ret),parent_done)
            elif variant=='child_duplicate':self.child.append(copy.deepcopy(child_done))
            elif variant=='child_early':self.child.remove(child_done);self.child.insert(0,child_done)
            elif variant=='return_kind':ret['payload']['type']='custom_tool_call_output'
            elif variant=='return_absent':self.parent.remove(ret)
            elif variant=='return_duplicate':self.parent.append(copy.deepcopy(ret))
            else:ret['payload']['output']=json.dumps({'task_name':'/root/wrong'})
            with self.subTest(variant=variant):
                if variant in ('parent_duplicate','child_duplicate','return_kind','return_absent','return_duplicate'):
                    with self.assertRaises(verify.EvidenceError):self.assess()
                else:self.assertEqual(self.assess()['status'],'behavioral_failure')
        self.parent,self.child=originalp,originalc

    def test_fifo_result_is_rejected_without_blocking(self):
        import os,time
        path=self.workspace/'RESULT.txt';path.unlink();os.mkfifo(path)
        start=time.monotonic();self.assertEqual(self.assess()['status'],'behavioral_failure');self.assertLess(time.monotonic()-start,1)

    def test_optional_observation_and_contradiction(self):
        self.child=[e for e in self.child if e['type']!='turn_context'];self.assertEqual(self.assess()['status'],'pass')
        self.child.insert(1,{'type':'turn_context','payload':{'model':'other','effort':'medium'}})
        result=self.assess();self.assertEqual(result['status'],'behavioral_failure');self.assertEqual(result['observed_thread_settings']['child']['model']['values'],['other']);self.assertIn('operator_question',result['disposition'])

class ReplayControls(unittest.TestCase):
    def test_preflight_required_for_whole_replay_pass(self):
        import replay_native_lineage as replay
        with tempfile.TemporaryDirectory() as tmp:
            evidence=Path(tmp)/'planar-t044-test';evidence.mkdir()
            trial=evidence/evidence.name/'plugin-smoke__test';trial.mkdir(parents=True);(trial/'result.json').write_text('{}')
            manifest=json.loads((replay.CASE/'candidate.json').read_text())
            for name in ('launcher.json','planar-install-provenance.json'):(evidence/name).write_text(json.dumps({'candidate_sha256':manifest['sha256']}))
            with patch.object(replay,'check'), patch.object(replay,'inventory',return_value=manifest), patch.object(replay.verify,'assess',side_effect=lambda *a,**kw:{'status':'pass'}):
                result=replay.replay(evidence);self.assertEqual(result['status'],'incomplete');self.assertEqual(result['recorded_lineage_status'],'pass')
                preflight=evidence/'planar-workspace-preflight.json'
                record={'execution_id':evidence.name,'status':'passed','fixture':'/workspace','check':'empty-directory-before-model'}
                preflight.write_text(json.dumps(record));self.assertEqual(replay.replay(evidence)['status'],'pass')
                record['execution_id']='wrong';preflight.write_text(json.dumps(record))
                with self.assertRaisesRegex(ValueError,'invalid workspace preflight'):replay.replay(evidence)

if __name__=='__main__':unittest.main()
