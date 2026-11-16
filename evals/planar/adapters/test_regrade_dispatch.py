import copy
import json
import unittest

from regrade_dispatch import CACHE, FILES, SKILLS, CANDIDATE, grade_rollouts


def response(kind, **fields):
    return {'type': 'response_item', 'payload': {'type': kind, **fields}}


def fixture():
    expected = {name: (CANDIDATE / name).read_text() for name in FILES}
    parent = [{'type':'session_meta','payload':{'id':'parent','source':'exec','originator':'codex_exec'}}, {'type':'turn_context','payload':{'model':'gpt-6.1-terra','effort':'medium'}}]
    skill_body = f'- `r0` = `{CACHE}/skills`\n' + '\n'.join(f'- planar:{name}: description (file: r0/{name}/SKILL.md)' for name in SKILLS)
    parent.append({'type':'world_state','payload':{'state':{'host_skills':{'body':skill_body}}}})
    for index,(name,text) in enumerate(expected.items()):
        call='r'+str(index); command=json.dumps('cat '+CACHE+'/'+name)
        parent.extend([response('custom_tool_call',name='exec',call_id=call,input='const r = await tools.exec_command({cmd:'+command+'}); text(r.output);'), response('custom_tool_call_output',call_id=call,output=[{'text':'Script completed\n'}, {'text':text}])])
    args={'task_name':'worker','fork_turns':'none','model':'gpt-6.1-terra','reasoning_effort':'medium','message':'encoded'}
    parent.extend([response('function_call',name='spawn_agent',namespace='collaboration',call_id='spawn',arguments=json.dumps(args)),response('function_call_output',call_id='spawn',output=json.dumps({'task_name':'/root/worker'}))])
    child = [
        {'type':'session_meta','payload':{'id':'child','parent_thread_id':'parent','source':{'subagent':{'thread_spawn':{'parent_thread_id':'parent','agent_path':'/root/worker','depth':1}}},'agent_path':'/root/worker'}},
        {'type':'turn_context','payload':{'model':'gpt-6.1-terra','effort':'medium'}},
        response('custom_tool_call',name='exec',call_id='write',input='const r = await tools.exec_command({cmd:' + json.dumps("printf '2\\n' > RESULT.txt") + ',"workdir":"/workspace"}); text(r.output);'),
        response('custom_tool_call_output',call_id='write',output=[{'text':'Script completed\n'}]),
        {'type':'event_msg','payload':{'type':'task_complete'}}]

    from datetime import datetime, timedelta, timezone
    base=datetime(2026,11,16,12,0,0,tzinfo=timezone.utc)
    for i,event in enumerate(parent): event['timestamp']=(base+timedelta(seconds=i)).isoformat()
    spawn_index=next(i for i,event in enumerate(parent) if event['payload'].get('name')=='spawn_agent')
    child_base=base+timedelta(seconds=spawn_index, milliseconds=500)
    for i,event in enumerate(child): event['timestamp']=(child_base+timedelta(seconds=i)).isoformat()
    return parent,child,expected


class RegradeControls(unittest.TestCase):
    def test_complete_evidence_keeps_role_delivery_unverified(self):
        result=grade_rollouts(*fixture())
        self.assertEqual(result['spawn_fresh_requested_pins'],'pass')
        self.assertEqual(result['host_skill_inventory'],'pass')
        self.assertEqual(result['role_instruction_delivery'],'unverified')

    def test_missing_child_mismatched_pin_and_forged_reads_fail(self):
        parent,child,expected=fixture()
        bad=copy.deepcopy(child);bad[0]['payload']['id']='parent'
        with self.assertRaises(ValueError): grade_rollouts(parent,bad,expected)
        bad=copy.deepcopy(parent); spawn=next(e for e in bad if e['payload'].get('name')=='spawn_agent'); args=json.loads(spawn['payload']['arguments']);args['fork_turns']='all';spawn['payload']['arguments']=json.dumps(args)
        with self.assertRaises(ValueError): grade_rollouts(bad,child,expected)
        bad=copy.deepcopy(parent); call=next(e for e in bad if e['payload'].get('call_id')=='r0' and e['payload'].get('type')=='custom_tool_call'); call['payload']['input']=call['payload']['input'].replace('cat ','echo ')
        with self.assertRaises(ValueError): grade_rollouts(bad,child,expected)

    def test_missing_discovery_is_separate_and_incomplete_read_fails(self):
        parent,child,expected=fixture()
        parent=[e for e in parent if e['type']!='world_state']
        self.assertEqual(grade_rollouts(parent,child,expected)['host_skill_inventory'],'pending')
        expected=dict(expected); expected[FILES[0]]+='tampered'
        with self.assertRaises(ValueError): grade_rollouts(parent,child,expected)

    def test_missing_completion_or_spawn_output_fail(self):
        parent,child,expected=fixture()
        with self.assertRaises(ValueError): grade_rollouts(parent,child[:-1],expected)
        parent=[e for e in parent if e['payload'].get('call_id')!='spawn' or e['payload'].get('type')!='function_call_output']
        with self.assertRaises(ValueError): grade_rollouts(parent,child,expected)

    def test_cached_result_and_candidate_mismatch_fail(self):
        import tempfile
        from pathlib import Path
        from unittest.mock import patch
        import regrade_dispatch as module
        parent, child, expected = fixture()
        manifest = json.loads(module.MANIFEST.read_text())
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / 'planar-t044-test'; evidence.mkdir()
            def write(name, obj): (evidence / name).write_text(json.dumps(obj))
            write('launcher.json', {'candidate_sha256':manifest['sha256']})
            write('planar-install-provenance.json', {'candidate_sha256':manifest['sha256']})
            write('planar-container.json', {'id':'container','image_id':module.EXPECTED_IMAGE})
            write('planar-preauth.json', {'status':'passed','container_id':'container','image_id':module.EXPECTED_IMAGE})
            write('planar-launch.json', {'command':'codex exec --model gpt-6.1-terra -c model_reasoning_effort=medium'})
            trial=evidence/evidence.name/'plugin-smoke__test'; trial.mkdir(parents=True)
            (trial/'result.json').write_text('{}')
            result=trial/'artifacts/workspace/RESULT.txt'; result.parent.mkdir(parents=True); result.write_bytes(b'2\n')
            sessions=trial/'agent/sessions/2026/11/16'; sessions.mkdir(parents=True)
            for name, rows in [('rollout-parent.jsonl', parent), ('rollout-child.jsonl', child)]:
                (sessions/name).write_text(''.join(json.dumps(row)+'\n' for row in rows))
            with patch.object(module, 'check_provenance'):
                self.assertEqual(module.regrade(evidence)['status'],'pass')
                result.write_bytes(b'2')
                with self.assertRaisesRegex(ValueError, 'bytes'): module.regrade(evidence)
                result.write_bytes(b'2\n')
                write('planar-install-provenance.json', {'candidate_sha256':'wrong'})
                with self.assertRaisesRegex(ValueError, 'candidate'): module.regrade(evidence)

    def test_read_only_unrelated_or_wrong_workdir_does_not_prove_write(self):
        parent, child, expected = fixture()
        for command in ('cat /workspace/RESULT.txt', 'echo /workspace/RESULT.txt', "printf '2\\n' > /tmp/RESULT.txt"):
            bad=copy.deepcopy(child)
            bad[2]['payload']['input']='const r = await tools.exec_command({cmd:'+json.dumps(command)+',"workdir":"/workspace"}); text(r.output);'
            with self.assertRaises(ValueError): grade_rollouts(parent,bad,expected)
        bad=copy.deepcopy(child)
        bad[2]['payload']['input']=bad[2]['payload']['input'].replace('/workspace','/tmp')
        with self.assertRaises(ValueError): grade_rollouts(parent,bad,expected)

    def test_child_activity_and_output_must_precede_completion(self):
        parent, child, expected = fixture()
        for order in ([0,1,4,2,3],[0,1,2,4,3]):
            with self.assertRaises(ValueError): grade_rollouts(parent,[child[i] for i in order],expected)

    def test_spawn_namespace_name_lineage_and_timestamps(self):
        parent, child, expected=fixture()
        for mutation in ('namespace','name','depth','path','stale','reverse'):
            p,c=copy.deepcopy(parent),copy.deepcopy(child)
            spawn=next(e for e in p if e['payload'].get('name')=='spawn_agent')
            if mutation=='namespace': spawn['payload']['namespace']='other'
            elif mutation=='name':
                args=json.loads(spawn['payload']['arguments']); args['task_name']='other'; spawn['payload']['arguments']=json.dumps(args)
            elif mutation=='depth': c[0]['payload']['source']['subagent']['thread_spawn']['depth']=2
            elif mutation=='path': c[0]['payload']['agent_path']='/root/other'
            elif mutation=='stale': c[0]['timestamp']='2025-01-01T00:00:00+00:00'
            else:
                position=p.index(spawn); p[position],p[position+1]=p[position+1],p[position]
            with self.assertRaises(ValueError, msg=mutation): grade_rollouts(p,c,expected)

    def test_nonexecuted_or_ambiguous_javascript_is_not_command_evidence(self):
        parent, child, expected=fixture()
        legit=child[2]['payload']['input']
        read='const r = await tools.exec_command({cmd:"cat /workspace/RESULT.txt", "workdir":"/workspace"}); text(r.output);'
        variants=[
            '// '+legit+'\n'+read,
            'const ignored = '+json.dumps(legit)+';\n'+read,
            'if (false) { '+legit+' }\n'+read,
            legit+'\n'+read,
            'const ignored = {cmd:"printf \'2\\\\n\' > RESULT.txt", "workdir":"/workspace"};\n'+read,
            legit.replace('tools.exec_command','tools.other_command'),
        ]
        for script in variants:
            bad=copy.deepcopy(child);bad[2]['payload']['input']=script
            with self.assertRaises(ValueError, msg=script): grade_rollouts(parent,bad,expected)

    def test_parent_read_comment_cannot_prove_installed_read(self):
        parent,child,expected=fixture()
        bad=copy.deepcopy(parent)
        call=next(e for e in bad if e['payload'].get('call_id')=='r0' and e['payload'].get('type')=='custom_tool_call')
        call['payload']['input']='// '+call['payload']['input']+'\nconst r = await tools.exec_command({cmd:"echo unrelated"}); text(r.output);'
        with self.assertRaises(ValueError): grade_rollouts(bad,child,expected)
