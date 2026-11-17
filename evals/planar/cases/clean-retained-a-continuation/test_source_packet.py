import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import source_packet


def data(value): return json.dumps(value,sort_keys=True).encode()


class ClosedSource(unittest.TestCase):
    def test_closed_packet_and_negative_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'packet'; root.mkdir(); eid='source'; job='job'; trial='trial'
            items={
                'semantic-audit.md':b'synthetic independent audit',
                'original-gate.json':data({'execution_id':eid}),
                'launcher.json':data({'execution_id':eid}),
                'native-trial-start.json':data({'job_id':job,'trial_id':trial}),
                'native-job-result.json':data({'id':job,'n_total_trials':1}),
                'native-trial-result.json':data({'id':trial,'config':{'job_id':job}}),
                'original-status.json':data({'status':'behavioral_failure'}),
                'original-reward.txt':b'0\n',
                'offline-regrade.json':data({'original_online_outcome':{'status':'behavioral_failure','reward':0},
                    'offline_assessment':{'status':'pass','semantic_audit':'required','lineage':{'role_delivery':'unverified'}}}),
            }
            release={'execution_id':eid,'terminal_class':'behavioral_failure','decision':'release-account','independent_review':True}
            census={'execution_id':eid,'native_group':'absent'}
            items['original-resolution-review.json']=data(release)
            items['original-resolution-census.json']=data(census)
            items['original-resolution.json']=data({'schema':'planar-clean-work-account-marker-v1','type':'clean-work-account-v1',
                'execution_id':eid,'review_sha256':hashlib.sha256(items['original-resolution-review.json']).hexdigest(),
                'census_sha256':hashlib.sha256(source_packet.canonical(census)).hexdigest(),'review_binding':release})
            files={}
            for name,value in items.items():
                path=root/'evidence'/name; path.parent.mkdir(exist_ok=True); path.write_bytes(value)
                files['evidence/'+name]={'size':len(value),'sha256':hashlib.sha256(value).hexdigest()}
            policy={'schema':'clean-retained-a-source-v1','execution':eid,'job':job,'trial':trial,'candidate_sha256':'candidate',
                    'source_online_status':'behavioral_failure','source_online_reward':0,'worker_role_delivery':'unverified',
                    'source_semantics':'packet-read-causal-write-completion-only','files':files}
            p=Path(tmp)/'policy.json'; p.write_bytes(data(policy))
            with patch.object(source_packet,'POLICY',p), patch.object(source_packet,'AUDIT',files['evidence/semantic-audit.md']['sha256']):
                self.assertEqual(source_packet.validate_closed(root,'candidate')['worker_role_delivery'],'unverified')
                with self.assertRaises(ValueError): source_packet.validate_closed(root,'different')
                policy['worker_role_delivery']='verified'; p.write_bytes(data(policy))
                with self.assertRaises(ValueError): source_packet.validate_closed(root,'candidate')
                policy['worker_role_delivery']='unverified'; p.write_bytes(data(policy))
                (root/'evidence/semantic-audit.md').write_bytes(b'tampered')
                with self.assertRaises(ValueError): source_packet.validate_closed(root,'candidate')
                (root/'evidence/semantic-audit.md').write_bytes(items['semantic-audit.md'])
                (root/'evidence/extra').write_bytes(b'extra')
                with self.assertRaises(ValueError): source_packet.validate_closed(root,'candidate')
                (root/'evidence/extra').unlink(); (root/'evidence/semantic-audit.md').unlink()
                (root/'evidence/semantic-audit.md').symlink_to(p)
                with self.assertRaises(ValueError): source_packet.validate_closed(root,'candidate')
