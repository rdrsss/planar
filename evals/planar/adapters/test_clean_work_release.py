import json
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch

import clean_work_release as release
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'experiments'))
import codex_run
from test_clean_work_release_schema import CleanWorkReleaseSchema


class CleanWorkPublication(TestCase):
    def test_shared_evidence_reader_rejects_traversal_and_symlinks(self):
        with TemporaryDirectory() as tmp:
            parent=Path(tmp).resolve(); root=parent/'reviews'; root.mkdir()
            inside=root/'authority.json'; inside.write_text('{}'); outside=parent/'authority.json'; outside.write_text('{}')
            link=root/'link'; link.symlink_to(inside)
            for path in (root/'..'/'authority.json',link):
                with self.subTest(path=str(path)):
                    with self.assertRaises(ValueError): release.read_safe(path,root)
            self.assertEqual(release.read_safe(inside,root),b'{}')

    def test_review_path_traversal_and_symlinks_are_rejected(self):
        with TemporaryDirectory() as tmp:
            parent=Path(tmp).resolve(); root=parent/'reviews'; root.mkdir(); jobs=root/'jobs'; jobs.mkdir()
            doc=CleanWorkReleaseSchema().document(); eid=doc['execution_id']; evidence=jobs/eid; evidence.mkdir()
            review=root/'review.json'; review.write_text(json.dumps(doc)); outside=parent/'outside.json'; outside.write_bytes(review.read_bytes())
            linked=root/'linked.json'; linked.symlink_to(review)
            linked_dir=root/'linked-dir'; linked_dir.symlink_to(root, target_is_directory=True)
            verified={'trial_name':'case__x','trial':{}}; census={'execution_id':eid,'native_group':'absent','matching_commands':0,'unresolved':0}
            identity={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}
            with patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs),patch.object(release.evidence_checks,'verify',return_value=verified),patch.object(release.clean_work_live,'check',return_value=census),patch.object(release,'resolver_identity',return_value=identity):
                with codex_run.admission_scope() as records:
                    start=records/(eid+'.start.json'); start.write_text(json.dumps({'case':'clean-work-review','execution_id':eid}))
                for bad in (root/'..'/'outside.json',linked,linked_dir/'review.json'):
                    with self.subTest(path=str(bad)):
                        with self.assertRaises(ValueError): release.publish(evidence,bad)
                        self.assertFalse((records/(eid+'.resolved.json')).exists())
                release.publish(evidence,review)
                marker=records/(eid+'.resolved.json'); original=json.loads(marker.read_text())
                for bad in (root/'..'/'outside.json',linked,linked_dir/'review.json'):
                    with self.subTest(consumer=str(bad)):
                        altered={**original,'review_path':str(bad)}; marker.write_bytes(release.canonical(altered)+b'\n')
                        with codex_run.admission_scope() as locked:
                            self.assertEqual(len(codex_run.shared_pending(locked)),1)

    def test_lock_publication_idempotence_and_consumer_review_binding(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs'; jobs.mkdir(); doc=CleanWorkReleaseSchema().document(); eid=doc['execution_id']; evidence=jobs/eid; evidence.mkdir(); review=root/'review.json'; review.write_text(json.dumps(doc));
            verified={'trial_name':'case__x','trial':{}}
            census={'execution_id':eid,'native_group':'absent','matching_commands':0,'unresolved':0}
            identity={'commit':doc['resolver_commit'],'implementation_sha256':doc['resolver_implementation_sha256']}
            with patch.object(release.launch,'reviews_root',return_value=root),patch.object(release.launch,'jobs_root',return_value=jobs),patch.object(release.evidence_checks,'verify',return_value=verified),patch.object(release.clean_work_live,'check',return_value=census),patch.object(release,'resolver_identity',return_value=identity):
                with codex_run.admission_scope() as records:
                    start=records/(eid+'.start.json'); start.write_text(json.dumps({'case':'clean-work-review','execution_id':eid}))
                with patch.object(release.os,'link',side_effect=OSError('injected publication failure')):
                    with self.assertRaises(OSError): release.publish(evidence,review)
                self.assertFalse((records/(eid+'.resolved.json')).exists())
                self.assertEqual(len(codex_run.shared_pending(records)),1)
                self.assertEqual(release.publish(evidence,review)['account'],'resolved')
                marker=records/(eid+'.resolved.json'); self.assertTrue(marker.exists())
                self.assertEqual(len(codex_run.shared_pending(records)),1)
                with codex_run.admission_scope() as locked:
                    self.assertEqual(codex_run.shared_pending(locked),[])
                self.assertEqual(release.publish(evidence,review)['account'],'resolved')
                with patch.object(release.clean_work_live,'check',side_effect=ValueError('resource alive')):
                    with codex_run.admission_scope() as locked:
                        self.assertEqual(len(codex_run.shared_pending(locked)),1)
                original_review=review.read_bytes(); review.write_bytes(original_review+b' ')
                with codex_run.admission_scope() as locked:
                    self.assertEqual(len(codex_run.shared_pending(locked)),1)
                review.write_bytes(original_review)
                data=json.loads(marker.read_text()); data['review_binding']['steps']['review']['reward']=False
                marker.write_bytes(release.canonical(data)+b'\n')
                with codex_run.admission_scope() as locked:
                    self.assertEqual(len(codex_run.shared_pending(locked)),1)
                with self.assertRaisesRegex(ValueError,'conflicting'): release.publish(evidence,review)
