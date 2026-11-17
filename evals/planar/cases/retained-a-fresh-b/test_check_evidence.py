import json
import unittest
from check_evidence import CHECK, WORKDIR, verify_two
from native import MissingEvidence, InvalidBehavior


def event(payload): return {'type':'response_item','payload':payload}
def scenario():
    rows=[event({'type':'function_call','name':'mark_start','call_id':'s1'}),
          event({'type':'function_call','name':'exec_command','call_id':'c1','arguments':json.dumps({'cmd':CHECK,'workdir':WORKDIR})}),
          event({'type':'function_call_output','call_id':'c1','output':json.dumps({'exit_code':0,'output':''})}),
          event({'type':'function_call','name':'spawn_agent','call_id':'r1'}),
          event({'type':'function_call','name':'mark_start','call_id':'s2'}),
          event({'type':'custom_tool_call','name':'exec','call_id':'c2','input':f'const r = await tools.exec_command({{cmd:{json.dumps(CHECK)}, workdir:{json.dumps(WORKDIR)}}}); text(JSON.stringify(r));'}),
          event({'type':'custom_tool_call_output','call_id':'c2','output':[{'text':'Script completed'}, {'text':json.dumps({'exit_code':0,'output':''})}]}),
          event({'type':'function_call','name':'spawn_agent','call_id':'r2'})]
    return rows,[(0,3),(4,7)]

class CheckControls(unittest.TestCase):
    def test_two_independent_calls_and_formats(self):
        rows,windows=scenario(); self.assertEqual(verify_two(rows,windows)['checks'],2)
    def test_failed_exit_and_nonempty_output_rejected(self):
        for value in ({'exit_code':1,'output':''},{'exit_code':0,'output':'failed'}):
            rows,w=scenario(); rows[2]['payload']['output']=json.dumps(value)
            with self.assertRaises(InvalidBehavior): verify_two(rows,w)
    def test_claimed_command_inside_arbitrary_code_is_not_execution(self):
        rows,w=scenario(); rows[5]['payload']['input']='text('+json.dumps(CHECK)+');'
        with self.assertRaises(MissingEvidence): verify_two(rows,w)
    def test_missing_output_or_ambiguous_calls_are_incomplete(self):
        rows,w=scenario(); rows[2]['payload']['output']='passed'
        with self.assertRaises(MissingEvidence): verify_two(rows,w)
        rows,w=scenario(); rows.insert(2,rows[1].copy())
        with self.assertRaises(MissingEvidence): verify_two(rows,[(0,4),(5,8)])
    def test_mismatched_native_output_type_is_incomplete(self):
        rows,w=scenario(); rows[2]['payload']['type']='custom_tool_call_output'
        with self.assertRaises(MissingEvidence): verify_two(rows,w)

    def test_check_must_precede_reviewer(self):
        rows,w=scenario(); rows[2],rows[3]=rows[3],rows[2]
        with self.assertRaises(InvalidBehavior): verify_two(rows,w)

if __name__ == '__main__': unittest.main()
