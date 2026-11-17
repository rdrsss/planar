import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import verify_restore


def metadata(data): return {'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}

class RestorationControls(unittest.TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name); self.target=self.root/'target'; self.target.mkdir()
        self.before={'home/projects/revision-fault/tasks/T-001/record.md':b'original',
            'repository/README.md':b'base','repository/RESULT.txt':b'2\n','repository/STATE.txt':b'correct\n',
            'repository/.git/index':b'not source index','host/git-head.txt':b'base','host/git-stage.txt':b'stage','host/git-status.txt':b'status'}
        self.before['host/repository-inventory.json']=json.dumps({'files':{'STATE.txt':metadata(b'correct\n')}}).encode()
        self.source=dict(self.before); self.source['repository/.git/index']=b'different internal index'
        self.after=dict(self.before); self.after['repository/STATE.txt']=b'fault-injected\n'
        self.after['host/repository-inventory.json']=json.dumps({'files':{'STATE.txt':metadata(b'fault-injected\n')}}).encode()
        self.clock=lambda n:{'stage':n,'wall_ns':{'pre':1,'restored-pre':2,'post':5}[n],'monotonic_ns':{'pre':1,'restored-pre':2,'post':5}[n]}
        change={'changed':'repository/STATE.txt','before':metadata(b'correct\n'),'after':metadata(b'fault-injected\n')}
        (self.target/'injection.json').write_text(json.dumps({'change':change,'started':{'wall_ns':3,'monotonic_ns':3},'ended':{'wall_ns':4,'monotonic_ns':4}}))
        self.patchers=[patch.object(verify_restore,'validate',return_value={'source_packet_sha256':'fixture'}),
            patch('capture.load',side_effect=lambda p: self.source if str(p).endswith('source/pre') else self.before if p.name=='restored-pre' else self.after),
            patch('chronology.receipt',side_effect=lambda root,name:self.clock(name))]
        for p in self.patchers: p.start(); self.addCleanup(p.stop)
    def run_verify(self): return verify_restore.verify(self.root/'packet',self.root/'candidate',self.target)
    def test_restored_source_and_sole_injection(self): self.assertEqual(self.run_verify()['target_b_assessment'],'not-implemented')
    def test_source_prefix_rewrite_or_extra_file_fails(self):
        name='home/projects/revision-fault/tasks/T-001/record.md'; self.before[name]=b'rewritten'
        with self.assertRaises(ValueError): self.run_verify()
        self.before[name]=b'original'; self.before['home/cache/leak']=b'x'
        with self.assertRaises(ValueError): self.run_verify()
    def test_nonsole_injection_and_wrong_receipt_fail(self):
        self.after['repository/RESULT.txt']=b'3\n'
        with self.assertRaises(ValueError): self.run_verify()
        self.after['repository/RESULT.txt']=b'2\n'; data=json.loads((self.target/'injection.json').read_text()); data['change']['changed']='repository/RESULT.txt'; (self.target/'injection.json').write_text(json.dumps(data))
        with self.assertRaises(ValueError): self.run_verify()
    def test_target_receipt_cannot_precede_source(self):
        with patch('chronology.receipt',side_effect=lambda root,name: {'wall_ns':2 if name == 'pre' else 1,'monotonic_ns':2 if name == 'pre' else 1}):
            with self.assertRaises(ValueError): self.run_verify()

    def test_git_stage_drift_fails(self):
        self.before['host/git-stage.txt']=b'other'
        with self.assertRaises(ValueError): self.run_verify()

if __name__ == '__main__': unittest.main()
