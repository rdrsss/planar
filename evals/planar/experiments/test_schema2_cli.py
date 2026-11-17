import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
import schema2
import per_variant
import schema2_reporting

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE.parent/'cases/native-lineage-portable'
PYTHON = HERE.parents[1]/'.venv/bin/python'

def when(seconds): return (datetime.now(timezone.utc)+timedelta(seconds=seconds)).isoformat()

class Schema2Cli(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.out = self.root/'experiment'
        variants=[]
        for name, effort in [('a','medium'),('b','high')]:
            path=self.root/name; (path/'.claude-plugin').mkdir(parents=True)
            (path/'.claude-plugin/plugin.json').write_text(json.dumps({'name':'planar','version':'0.1.0'}))
            for skill in schema2.SKILLS:
                d=path/'skills'/skill; d.mkdir(parents=True); (d/'SKILL.md').write_text(name)
            variants.append({'id':name,'candidate_source':str(path),'effort':effort,'image_id':'sha256:'+'a'*64,'verifier_image_id':'sha256:'+'b'*64,'host_version':'test-host'})
        definition={'schema':2,'template':str(TEMPLATE),'reviewed_template_sha256':per_variant.tree_hash(TEMPLATE),'variants':variants,'repetitions':2,'config':{'model':'gpt-6.1-terra','variant_efforts':{'a':'medium','b':'high'},'native_config':{'fixture':'synthetic'},'caps':{'session_seconds':5,'verifier_seconds':5,'trial_seconds':5,'job_seconds':3600,'trials':4,'max_executions':6}},'implementation_sha256':'c'*64,'deadline_at':when(3600),'account_scope_id':'scratch'}
        self.definition=self.root/'definition.json'; self.definition.write_text(json.dumps(definition))

    def cli(self, *args, ok=True):
        result=subprocess.run([str(PYTHON),str(HERE/'cli.py'),*map(str,args)],capture_output=True,text=True)
        if ok: self.assertEqual(result.returncode,0,result.stderr)
        else: self.assertNotEqual(result.returncode,0)
        return (json.loads(result.stdout) if result.stdout.strip() else None) if ok else result

    def test_plan_reserve_inspect_report_and_gate(self):
        manifest=self.cli('plan-v2',self.definition,self.out); identity=manifest['identity']
        selected=self.cli('preflight-v2',self.out,identity,'b:2')
        self.assertEqual(selected['trial']['variant_id'],'b')
        reservation=self.cli('reserve-v2',self.out,identity,'b:2','--expires-at',when(600))
        report=self.cli('report-v2',self.out,identity)
        self.assertEqual(report['groups'][1]['primary_reserved'],1)
        self.assertEqual(report['groups'][1]['primary_not_started'],2)
        self.assertEqual(report['groups'][1]['primary_native_start_record_present_unverified'],0)
        self.assertEqual(report['trials'][3]['attempts'][0]['state'],'planned')
        r=reservation; gate={k:r[k] for k in schema2.GATE_FIELDS if k in r}
        gate.update({'schema_version':2,'purpose':'one-experiment-trial','verdict':'approve','max_attempts':1,'reservation_sha256':schema2.h((self.out/'reservations'/f"{r['reservation_id']}.json").read_bytes()),'reviewer':'independent','report':'independent.md','reviewed_at':when(-1)})
        gatefile=self.root/'gate.json'; gatefile.write_text(json.dumps(gate))
        self.assertEqual(self.cli('check-gate-v2',self.out,identity,r['reservation_id'],gatefile)['authorization'],'not granted')
        self.assertTrue(gatefile.exists()); self.assertFalse((self.out/'executions'/r['execution_id']).exists())
        gate['image_id']='sha256:'+'e'*64; gatefile.write_text(json.dumps(gate))
        self.cli('check-gate-v2',self.out,identity,r['reservation_id'],gatefile,ok=False)
        self.cli('reserve-v2',self.out,identity,'b:2','--expires-at',when(600),ok=False)
        (self.out/'variants/b/candidate/skills/run/SKILL.md').write_text('changed')
        self.cli('preflight-v2',self.out,identity,'b:2',ok=False)

    def test_report_rejects_contradictory_reservation_and_execution(self):
        m=self.cli('plan-v2',self.definition,self.out); identity=m['identity']
        r=self.cli('reserve-v2',self.out,identity,'a:1','--expires-at',when(600))
        path=self.out/'reservations'/f"{r['reservation_id']}.json"
        original=path.read_bytes()
        for key in (*schema2.IDENTITIES, 'configuration_sha256', 'implementation_sha256', 'image_id', 'verifier_image_id', 'host_version', 'account_scope_id', 'canonical_output_root', 'state', 'expires_at', 'retry_reason_sha256'):
            changed=dict(r); changed[key] = when(7200) if key == 'expires_at' else 'wrong'
            path.write_bytes(per_variant.canonical(changed))
            with self.subTest(field=key), self.assertRaises(schema2.Invalid): schema2_reporting.report(self.out,identity)
            self.assertEqual(path.read_bytes(),per_variant.canonical(changed))
        path.write_bytes(original)
        folder=self.out/'executions'/r['execution_id']; folder.mkdir()
        start={'execution_id':r['execution_id'],'logical_id':'a:1'}
        for key in ('variant_id','repetition','reservation_id','predecessor_execution_id','candidate_snapshot_sha256','image_id'):
            wrong=dict(start); wrong[key]='wrong'; (folder/'start.json').write_bytes(per_variant.canonical(wrong))
            with self.subTest(start=key), self.assertRaises(schema2.Invalid): schema2_reporting.report(self.out,identity)
        (folder/'start.json').write_bytes(per_variant.canonical(start))
        native=folder/'native-start.json'; native.write_bytes(per_variant.canonical({'execution_id':'wrong'}))
        with self.assertRaises(schema2.Invalid): schema2_reporting.report(self.out,identity)
        native.write_bytes(per_variant.canonical({'opaque':'unverified'}))
        attempt=schema2_reporting.report(self.out,identity)['trials'][0]['attempts'][0]
        self.assertEqual(attempt['state'],'execution_record_present_unverified')
        self.assertTrue(attempt['native_start_record_present_unverified'])
        native.unlink(); native.symlink_to(path)
        with self.assertRaises(schema2.Invalid): schema2_reporting.report(self.out,identity)

    def test_report_retains_retry_without_inventing_verdict(self):
        m=self.cli('plan-v2',self.definition,self.out); identity=m['identity']
        primary=self.cli('reserve-v2',self.out,identity,'a:1','--expires-at',when(600))
        folder=self.out/'executions'/primary['execution_id']; folder.mkdir()
        schema2.write_new(folder/'start.json',{'execution_id':primary['execution_id'],'logical_id':'a:1'})
        schema2.write_new(folder/'terminal.json',{'status':'incomplete'})
        retry=self.cli('reserve-v2',self.out,identity,'a:1','--expires-at',when(600),'--retry-of',primary['execution_id'],'--retry-reason','interrupted')
        report=self.cli('report-v2',self.out,identity)
        row=report['trials'][0]
        self.assertEqual(len(row['attempts']),2)
        self.assertEqual(next(a for a in row['attempts'] if a['reservation_id']==retry['reservation_id'])['state'],'planned')
        self.assertEqual(report['groups'][0]['retry_attempts'],1)
        self.assertEqual(report['groups'][0]['primary_not_started'],1)
        self.assertEqual(report['groups'][0]['all_attempts_reserved'],2)
        self.assertEqual(report['groups'][0]['all_attempts_planned'],1)
        self.assertIn('no behavioral verdict',report['assessment'])
        dest=self.root/'report.json'; self.cli('report-v2',self.out,identity,'--output',dest)
        self.cli('report-v2',self.out,identity,'--output',dest,ok=False)

if __name__ == '__main__': unittest.main()
