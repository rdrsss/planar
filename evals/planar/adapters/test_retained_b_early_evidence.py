import json
import hashlib
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch
import launch
import retained_b_early_evidence as evidence

PROJECT=launch.PROJECT
PREP=PROJECT/'reviews/t045-retained-b-final-prep-20261117'
JOB=PREP/'output/jobs'/evidence.EID
CASE=PREP/'source/evals/planar/cases/retained-a-fresh-b'
CANDIDATE=PREP/'source/candidate'
PACKET=PROJECT/'reviews/t045-retained-a-fresh-b-offline-20261117/packet-v2'
AUDIT=PROJECT/'reviews/t045-retained-b-live-root-audit-evidence-20261117.json'

class EarlyEvidence(TestCase):
    def setUp(self):
        if not AUDIT.is_file(): self.skipTest('retained reviewed evidence unavailable')
        self.env=patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(PREP/'output')}); self.env.start(); self.addCleanup(self.env.stop)
        self.source=patch.object(evidence,'source',return_value={'source_packet_sha256':evidence.SOURCE_SHA}); self.source.start(); self.addCleanup(self.source.stop)
    def inspect(self): return evidence.inspect(JOB,CASE,CANDIDATE,PACKET,AUDIT)
    def test_actual_closed_evidence_remains_pending(self):
        out=self.inspect(); self.assertEqual(out['observed_status']['status'],'behavioral_failure'); self.assertEqual(out['reward'],0); self.assertEqual(out['account'],'pending')
    def test_wrong_audit_source_case_and_candidate_fail(self):
        for name,patcher in (('audit',patch.object(evidence,'AUDIT_SHA','bad')),('source',patch.object(evidence,'source',return_value={'source_packet_sha256':'bad'})),('case',patch.object(launch,'case_inventory',return_value='bad')),('candidate',patch.object(evidence,'inventory',return_value={'sha256':'bad'}))):
            with self.subTest(name=name),patcher,self.assertRaises(ValueError): self.inspect()
    def test_read_safe_refuses_links_and_escape(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); plain=root/'file'; plain.write_text('safe')
            link=root/'link'; link.symlink_to(plain)
            with self.assertRaises(ValueError): evidence.read_safe(link,root)
            with self.assertRaises(ValueError): evidence.read_safe(root.parent/'elsewhere',root)
    def test_audit_requires_true_boolean_provenance(self):
        original=json.loads(AUDIT.read_bytes()); real=evidence.read_safe
        for key in ('full_runtime_authority_revalidated','native_identity_equal','submitted_actual_jobconfigs_equal'):
            for value in (1,False,'true'):
                raw=json.dumps({**original,key:value}).encode()
                def altered(path,root,limit=4*1024*1024): return raw if Path(path)==AUDIT else real(path,root,limit)
                with self.subTest(key=key,value=value),patch.object(evidence,'AUDIT_SHA',hashlib.sha256(raw).hexdigest()),patch.object(evidence,'read_safe',side_effect=altered):
                    with self.assertRaisesRegex(ValueError,'provenance'): self.inspect()
    def test_gate_and_status_tampering_fails(self):
        real=evidence.read_safe
        for target in ('approved-review.json','status.json'):
            def altered(path,root,limit=4*1024*1024):
                raw=real(path,root,limit)
                if Path(path).name==target: return raw+b' '
                return raw
            with self.subTest(target=target),patch.object(evidence,'read_safe',side_effect=altered),self.assertRaises(ValueError): self.inspect()
