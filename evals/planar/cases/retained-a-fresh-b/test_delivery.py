import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from delivery import verify_child_read, HOME, PLUGIN
from native import MissingEvidence, InvalidBehavior


def events(filename,packet,refs):
    cmd=f'cat {PLUGIN}references/packets.md {PLUGIN}references/plans.md {HOME}{filename}'
    script='const r = await tools.exec_command({cmd:'+json.dumps(cmd)+',"workdir":"/workspace/repository","max_output_tokens":30000}); text(r.output)'
    call={'type':'response_item','payload':{'type':'custom_tool_call','name':'exec','call_id':'c','input':script}}
    out={'type':'response_item','payload':{'type':'custom_tool_call_output','call_id':'c','output':[{'text':'Script completed\n'},{'text':refs+packet}]}}
    call['timestamp']='2026-01-01T00:00:01Z'; out['timestamp']='2026-01-01T00:00:02Z'
    done={'type':'event_msg','timestamp':'2026-01-01T00:00:03Z','payload':{'type':'task_complete'}}
    return [call,out,done]

class DeliveryControls(unittest.TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.candidate=Path(self.tmp.name)
        (self.candidate/'references').mkdir(); (self.candidate/'references/packets.md').write_text('packets\n'); (self.candidate/'references/plans.md').write_text('plans\n')
        self.packet='full reviewer packet\n'; self.filename='reviewer-packet-1.md'; self.rows=events(self.filename,self.packet,'packets\nplans\n')
    def test_observed_host_multicat_form(self):
        self.assertEqual(verify_child_read(self.rows,self.filename,self.packet,self.candidate)['call_id'],'c')
    def test_bounded_sed_and_head_forms(self):
        for command in (f"sed -n '1,200p' {HOME}{self.filename}",f'head -c 10000 {HOME}{self.filename}'):
            rows=events(self.filename,self.packet,'packets\nplans\n')
            rows[0]['payload']={'type':'function_call','name':'exec_command','call_id':'c','arguments':json.dumps({'cmd':command,'workdir':'/workspace/repository'})}
            rows[1]['payload']={'type':'function_call_output','call_id':'c','output':json.dumps({'output':self.packet,'exit_code':0})}
            with self.subTest(command=command): self.assertEqual(verify_child_read(rows,self.filename,self.packet,self.candidate)['call_id'],'c')

    def test_parent_read_and_summary_are_not_child_delivery(self):
        child=[{'type':'response_item','payload':{'type':'message','role':'user','content':'summary only'}}]
        with self.assertRaises(MissingEvidence): verify_child_read(child,self.filename,self.packet,self.candidate)
    def test_packet_read_after_child_completion_is_rejected(self):
        self.rows[1],self.rows[2]=self.rows[2],self.rows[1]
        with self.assertRaises(InvalidBehavior): verify_child_read(self.rows,self.filename,self.packet,self.candidate)

    def test_child_output_must_contain_exact_full_packet(self):
        self.rows[1]['payload']['output'][1]['text']='packets\nplans\nsummary\n'
        with self.assertRaises(MissingEvidence): verify_child_read(self.rows,self.filename,self.packet,self.candidate)
    def test_unrecognized_or_write_call_before_read_is_incomplete(self):
        self.rows.insert(0,{'type':'response_item','payload':{'type':'custom_tool_call','name':'exec','call_id':'write','input':'text("pretend");'}})
        with self.assertRaises(MissingEvidence): verify_child_read(self.rows,self.filename,self.packet,self.candidate)
    def test_command_in_arbitrary_js_is_not_a_read(self):
        self.rows[0]['payload']['input']='text('+json.dumps(self.packet)+');'
        with self.assertRaises(MissingEvidence): verify_child_read(self.rows,self.filename,self.packet,self.candidate)

if __name__=='__main__': unittest.main()
