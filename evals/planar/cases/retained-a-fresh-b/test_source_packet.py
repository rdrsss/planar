import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import source_packet


class PacketControls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)/'packet'; self.root.mkdir()
        ids = {'candidate_sha256':'c','case_sha256':'k','implementation_sha256':'i','configuration_sha256':'g'}
        execution = 'planar-t044-test'
        values = {
            'source/initial/file': b'fixture',
            'evidence/native-trial-start.json': json.dumps({'job_id':'j','trial_id':'t','trial_name':'trial'}).encode(),
            'evidence/archived-job-result.json': json.dumps({'id':'j','n_total_trials':1}).encode(),
            'evidence/archived-trial-result.json': json.dumps({'id':'t','trial_name':'trial','config':{'job_id':'j'},'verifier_result':{'rewards':{'reward':0.0}}}).encode(),
            'evidence/original-gate.json': json.dumps({**ids,'execution_id':execution}).encode(),
            'evidence/job-config.json': b'job',
            'evidence/launcher.json': json.dumps({**ids,'execution_id':execution,'job_config_sha256':hashlib.sha256(b'job').hexdigest()}).encode(),
            'evidence/original-status.json': b'{"status":"behavioral_failure"}',
            'evidence/original-reward.txt': b'0\n',
        }
        files = {}
        for name, data in values.items():
            path=self.root/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_bytes(data)
            files[name]={'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}
        self.policy=Path(self.tmp.name)/'policy.json'
        self.policy.write_text(json.dumps({'schema':'retained-a-fresh-b-source-v1',**ids,'execution':execution,'job':'j','trial':'t','source_status':'behavioral_failure','assessor_image':'sha256:test','files':files}))
        self.assessment={'status':'pass','semantic_audit':'required','lineage':{'parents':['01a1e776-1d33-71d3-9a40-cda0ac5b2f08'],'children':['01a1e778-216b-7811-a3eb-5e095361b60d']}}
        self.patches=[patch.object(source_packet,'POLICY',self.policy),patch('candidate.inventory',return_value={'sha256':'c'}),
                      patch.object(source_packet.subprocess,'run',return_value=SimpleNamespace(stdout=json.dumps(self.assessment)))]
        for item in self.patches: item.start()
        self.addCleanup(self.tmp.cleanup)
        for item in self.patches: self.addCleanup(item.stop)

    def validate(self): return source_packet.validate(self.root, self.root)
    def test_closed_packet_and_isolated_oracle(self):
        self.assertEqual(self.validate()['assessment']['status'],'pass')
        call=source_packet.subprocess.run.call_args.args[0]
        self.assertIn('--network',call); self.assertIn('none',call); self.assertIn('--read-only',call); self.assertIn('never',call)
    def test_extra_missing_and_drift_refused(self):
        path=self.root/'source/initial/file'; old=path.read_bytes()
        path.write_bytes(b'changed')
        with self.assertRaises(ValueError): self.validate()
        path.write_bytes(old); path.unlink()
        with self.assertRaises(ValueError): self.validate()
        path.write_bytes(old); (self.root/'extra').write_bytes(b'x')
        with self.assertRaises(ValueError): self.validate()
    def test_links_and_extra_directories_refused(self):
        path=self.root/'source/initial/file'; path.unlink(); path.symlink_to(self.policy)
        with self.assertRaises(ValueError): self.validate()
        path.unlink(); path.write_bytes(b'fixture'); (self.root/'unused').mkdir()
        with self.assertRaises(ValueError): self.validate()
    def test_bad_online_status_or_lineage_refused(self):
        self.assessment['lineage']['children'] = ['wrong']
        source_packet.subprocess.run.return_value=SimpleNamespace(stdout=json.dumps(self.assessment))
        with self.assertRaises(ValueError): self.validate()
        (self.root/'evidence/original-reward.txt').write_text('1\n')
        with self.assertRaises(ValueError): self.validate()

    def test_archived_trial_job_disagreement_refused_even_if_rehashed(self):
        name='evidence/archived-trial-result.json'; path=self.root/name
        data=json.loads(path.read_text()); data['config']['job_id']='different'; raw=json.dumps(data).encode(); path.write_bytes(raw)
        policy=json.loads(self.policy.read_text()); policy['files'][name]={'size':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}; self.policy.write_text(json.dumps(policy))
        with self.assertRaises(ValueError): self.validate()

    def test_bad_candidate_or_assessment_refused(self):
        with patch('candidate.inventory',return_value={'sha256':'wrong'}), self.assertRaises(ValueError): self.validate()
        source_packet.subprocess.run.return_value=SimpleNamespace(stdout=json.dumps({'status':'pass','lineage':{}}))
        with self.assertRaises(ValueError): self.validate()

if __name__ == '__main__': unittest.main()
