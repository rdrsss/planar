import hashlib
import json
import subprocess
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch

import launch
import retained_b_resolve as resolver

class Resolution(TestCase):
    def setUp(self):
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name); self.eid='planar-t044-'+'a'*32; self.source='b'*64
        self.env=patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(self.root)}); self.env.start(); self.addCleanup(self.env.stop)
        self.evidence=self.root/'jobs'/self.eid; self.evidence.mkdir(parents=True)
        self.records=self.root/'planar-account-admission-v1'/'records'; self.records.mkdir(parents=True)
        self.gate=self.root/'gate.json'; self.gate.write_text(json.dumps({'image_id':'i','verifier_image_id':'v','absolute_deadline':'x'}))
        (self.evidence/'retained-admission.json').write_text(json.dumps({'gate_path':str(self.gate),'source_packet_root':str(self.root),'source_packet_sha256':self.source}))
        for name in ('watchdog-terminal.json','launcher-terminal.json'): (self.evidence/name).write_text(json.dumps({'execution_id':self.eid}))
        self.status=self.evidence/self.eid/'trial/verifier/status.json'; self.status.parent.mkdir(parents=True)
        self.status.write_text(json.dumps({'status':'incomplete','file_structure':'pass','structural_checks':{},'semantic_audit':'required','source_packet_sha256':self.source})); self.status.with_name('reward.txt').write_text('0')
        registry=self.evidence/'projects'; registry.mkdir(); (registry/'one.json').write_text(json.dumps({'execution_id':self.eid,'compose_project':self.eid+'-env'}))
        (self.records/(self.eid+'.start.json')).write_text(json.dumps({'execution_id':self.eid,'case':'retained-a-fresh-b','gate_sha256':resolver.digest(self.gate),'gate_authority':{'source_packet_sha256':self.source},'status':'awaiting-independent-bound-raw-audit'}))
        self.audit=self.root/'independent.json'
        self.doc={'execution_id':self.eid,'decision':'release-account','behavior':'incomplete','gate_sha256':resolver.digest(self.gate),'source_packet_sha256':self.source,'verifier_status_sha256':resolver.digest(self.status),'reward_sha256':resolver.digest(self.status.with_name('reward.txt')),'reason':'reviewed cleanup and retained evidence'}
        self.audit.write_text(json.dumps(self.doc))
        self.runtime=patch.object(resolver,'verify_runtime'); self.runtime.start(); self.addCleanup(self.runtime.stop)
        real_run=subprocess.run
        def inspect(argv, **kwargs):
            if argv[0]=='git': return real_run(argv, **kwargs)
            if argv[0]!='docker': raise AssertionError('unexpected subprocess')
            return self.docker_mock.return_value
        self.docker=patch.object(resolver.subprocess,'run',side_effect=inspect,return_value=SimpleNamespace(returncode=0,stdout='')); self.docker_mock=self.docker.start(); self.addCleanup(self.docker.stop)
    def call(self): return resolver.resolve(self.evidence,self.root,self.root,self.audit)
    def test_explicit_bound_release_keeps_incomplete(self):
        self.assertEqual(self.call()['behavior'],'incomplete')
        self.assertEqual(json.loads((self.records/(self.eid+'.resolved.json')).read_text())['behavior'],'incomplete')
        self.assertEqual(self.call()['behavior'],'incomplete')
    def test_partial_publication_stays_pending_and_invalid_marker_conflicts(self):
        import codex_run
        with patch.object(resolver.os,'link',side_effect=OSError('interrupted before publish')):
            with self.assertRaises(OSError): self.call()
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        marker=self.records/(self.eid+'.resolved.json'); marker.write_text('{')
        self.assertEqual(len(codex_run.shared_pending(self.records)),1)
        with self.assertRaisesRegex(ValueError,'conflicting resolution'): self.call()
        self.assertEqual(marker.read_text(),'{')
        marker.unlink(); self.call()
        self.assertEqual(codex_run.shared_pending(self.records),[])
    def test_bad_binding_or_cleanup_never_releases(self):
        for fault in ('audit','cleanup','reward','docker'):
            with self.subTest(fault=fault):
                if fault=='audit': self.audit.write_text(json.dumps({**self.doc,'gate_sha256':'wrong'}))
                if fault=='cleanup': (self.evidence/'recovery-required').write_text('pending')
                if fault=='reward': self.status.with_name('reward.txt').write_text('1')
                if fault=='docker': self.docker_mock.return_value=SimpleNamespace(returncode=0,stdout='present')
                with self.assertRaises(ValueError): self.call()
                self.assertFalse((self.records/(self.eid+'.resolved.json')).exists())
                self.audit.write_text(json.dumps(self.doc)); (self.evidence/'recovery-required').unlink(missing_ok=True); self.status.with_name('reward.txt').write_text('0'); self.docker_mock.return_value=SimpleNamespace(returncode=0,stdout='')
