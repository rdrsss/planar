"""No-auth delivery and stale-gate controls for the operator confirmation."""
import json
from pathlib import Path
import sys
import unittest

from harbor.models.task.task import Task

CASE=Path(__file__).resolve().parent
ADAPTERS=CASE.parents[1]/'adapters'
sys.path.insert(0,str(ADAPTERS))
import launch
from retained_b_support import validate_gate

STATEMENT=('Operator confirmation for retained source execution '
           '`planar-t044-a8f4f54883f94e43bf18e7428e71bef1`: the previous source driver and all its children have stopped. '
           'This confirmation concerns the completed source execution, not this new target session; individual thread close was unsupported.')
OLD_CASE='90141823bb3ea4c18f47de66d9dba47e730dfb971bf65a615342ec009f031444'

class SourceStopInstruction(unittest.TestCase):
    def test_native_task_delivers_exact_confirmation_once(self):
        instruction=(CASE/'instruction.md').read_text()
        self.assertEqual(instruction.count(STATEMENT),1)
        self.assertEqual(Task(CASE).instruction,instruction)
        config=launch.config_for('fixture-job',Path('/tmp/fixture-job'),CASE,'medium')
        self.assertEqual(config['tasks'][0]['path'],str(CASE))
        self.assertEqual(config['agents'][0]['model_name'],'gpt-6.1-terra')
        self.assertEqual(config['agents'][0]['kwargs']['reasoning_effort'],'medium')
    def test_consumed_old_gate_refuses_changed_case_identity(self):
        old=launch.PROJECT/'reviews/t045-retained-b-final-prep-20261117/output/approved-review.json'
        if not old.is_file(): self.skipTest('historical gate unavailable')
        gate=json.loads(old.read_text()); policy=json.loads((CASE/'source-policy.json').read_text())
        self.assertEqual(gate['case_sha256'],OLD_CASE)
        current=launch.case_inventory(CASE); self.assertNotEqual(current,OLD_CASE)
        snapshot={k:gate[k] for k in ('candidate_sha256','implementation_sha256','configuration_sha256','image_id')}
        snapshot['case_sha256']=current
        snapshot['prerequisites']={k:gate[k] for k in ('effective_configuration_sha256','source_packet_sha256','source_packet_root')}
        with self.assertRaisesRegex(ValueError,'binding mismatch'):
            validate_gate(gate,snapshot,policy,gate['execution_id'],gate['absolute_deadline'],Path(gate['auth_path']),Path(gate['account_scope_id']),gate['verifier_image_id'])

if __name__=='__main__': unittest.main()
