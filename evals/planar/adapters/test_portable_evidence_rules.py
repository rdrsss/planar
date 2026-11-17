"""Synthetic general rules; not evidence of any historical execution."""
import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import retained_b_early_evidence as evidence
import retained_b_release_schema as release_schema


class PortableEvidenceRules(unittest.TestCase):
    def test_read_safe_rejects_links_and_escape(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); file=root/'file'; file.write_bytes(b'synthetic')
            link=root/'link'; link.symlink_to(file)
            self.assertEqual(evidence.read_safe(file,root),b'synthetic')
            for path in (link, root/'..'/'outside'):
                with self.subTest(path=path), self.assertRaises(ValueError): evidence.read_safe(path,root)

    def test_provenance_requires_true_booleans(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); reviews=root/'reviews'; reviews.mkdir(); jobs=root/'jobs'; jobs.mkdir()
            job=jobs/evidence.EID; job.mkdir(); audit=reviews/'synthetic.json'
            doc={'execution_id':evidence.EID,'job_id':evidence.JOB,'trial_id':evidence.TRIAL,
                 'source_packet_sha256':evidence.SOURCE_SHA,'candidate_sha256':evidence.CANDIDATE_SHA,
                 'case_sha256':evidence.CASE_SHA,'observed_verifier_status':{'status':'behavioral_failure','reason':'final record field mismatch'},
                 'observed_reward':'0','full_runtime_authority_revalidated':True,'native_identity_equal':True,'submitted_actual_jobconfigs_equal':True}
            for field in ('full_runtime_authority_revalidated','native_identity_equal','submitted_actual_jobconfigs_equal'):
                for value in (1,False,'true'):
                    raw=json.dumps({**doc,field:value}).encode(); audit.write_bytes(raw)
                    with self.subTest(field=field,value=value), patch.object(evidence.launch,'project_dir',return_value=root), \
                         patch.object(evidence.launch,'reviews_root',return_value=reviews), patch.object(evidence.launch,'jobs_root',return_value=jobs), \
                         patch.object(evidence,'AUDIT_SHA',hashlib.sha256(raw).hexdigest()):
                        with self.assertRaisesRegex(ValueError,'provenance'): evidence.inspect(job,root,root,root,audit)

    def test_review_schema_is_closed_and_booleans_are_strict(self):
        doc={key:'synthetic' for key in release_schema.FIELDS-release_schema.BOOLEANS}
        doc.update({key:True for key in release_schema.BOOLEANS})
        doc.update(type='aborted-before-work',decision='release-account',claim=release_schema.CLAIM,reviewed_at='2026-01-01T00:00:00+00:00')
        self.assertEqual(release_schema.parse(json.dumps(doc)),doc)
        for field in release_schema.BOOLEANS:
            for value in (False,1,'true',None):
                with self.subTest(field=field,value=value), self.assertRaises(ValueError):
                    release_schema.parse(json.dumps({**doc,field:value}))
        with self.assertRaises(ValueError): release_schema.parse(json.dumps({**doc,'extra':True}))
