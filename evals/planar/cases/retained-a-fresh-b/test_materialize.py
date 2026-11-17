import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import materialize


def meta(data): return {'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}

class MaterializeControls(unittest.TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name)
        self.data={'home/settings.yaml':b'settings','repository/RESULT.txt':b'2\n','repository/STATE.txt':b'correct\n'}
        policy={'files':{'source/pre/'+k:meta(v) for k,v in self.data.items()}}
        policy['files']['source/pre/host/git-stage.txt']=meta(b'stage')
        self.policy=self.root/'policy.json'; self.policy.write_text(json.dumps(policy)); self.digest=hashlib.sha256(json.dumps(policy,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()).hexdigest()
        p=patch.object(materialize,'POLICY',self.policy); p.start(); self.addCleanup(p.stop)
        self.durable=self.root/'durable-policy.json'; self.durable.write_text(json.dumps({'schema':'retained-b-durable-v1','source_packet_sha256':self.digest,'base':materialize.BASE,'stage':meta(b'stage'),'files':{k:meta(v) for k,v in self.data.items()}}))
        p=patch.object(materialize,'DURABLE_POLICY',self.durable); p.start(); self.addCleanup(p.stop)
        p=patch('source_packet.validate',return_value={'source_packet_sha256':self.digest}); p.start(); self.addCleanup(p.stop)
        p=patch('capture.load',return_value={**self.data,'logs/provider.jsonl':b'secret','repository/.git/index':b'index'}); p.start(); self.addCleanup(p.stop)
    def test_prepare_projects_only_durable_allowlist(self):
        dest=self.root/'durable'; result=materialize.prepare(self.root/'packet',self.root/'candidate',dest)
        self.assertEqual(result['files'],3); self.assertFalse((dest/'logs').exists()); self.assertFalse((dest/'repository/.git').exists())
    def test_prepare_refuses_drift_or_existing_destination(self):
        with patch('capture.load',return_value={**self.data,'repository/STATE.txt':b'other'}), self.assertRaises(ValueError): materialize.prepare(self.root/'p',self.root/'c',self.root/'durable')
        (self.root/'durable').mkdir()
        with self.assertRaises(ValueError): materialize.prepare(self.root/'p',self.root/'c',self.root/'durable')
    def test_install_fails_before_base_on_missing_or_extra_or_link(self):
        dest=self.root/'durable'; materialize.prepare(self.root/'p',self.root/'c',dest)
        target=self.root/'workspace'; target.mkdir()
        with patch('install_fixture.install') as install_base:
            path=dest/'home/settings.yaml'; path.unlink()
            with self.assertRaises(ValueError): materialize.install(dest,target,self.digest)
            path.write_bytes(b'settings'); (dest/'unexpected').write_bytes(b'x')
            with self.assertRaises(ValueError): materialize.install(dest,target,self.digest)
            (dest/'unexpected').unlink(); path.unlink(); path.symlink_to(self.policy)
            with self.assertRaises(ValueError): materialize.install(dest,target,self.digest)
            install_base.assert_not_called()
    def test_wrong_binding_rejected_before_restore(self):
        dest=self.root/'durable'; materialize.prepare(self.root/'p',self.root/'c',dest); target=self.root/'workspace'; target.mkdir()
        with patch('install_fixture.install') as install_base, self.assertRaises(ValueError): materialize.install(dest,target,'wrong')
        install_base.assert_not_called()

if __name__=='__main__': unittest.main()
