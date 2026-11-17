"""Read-only T-046 compatibility checks; no marker or gate is changed."""
import fcntl
import hashlib
import os
from pathlib import Path
import unittest
from unittest.mock import patch
import codex_run
import schema2_pretrial_release as release

@unittest.skipUnless(os.environ.get("PLANAR_EVALS_PROJECT_DIR") and os.environ.get("PLANAR_EVALS_LAYOUT_KIND") == "override-jobs", "requires explicit historical test selection")
class PublishedHistorical(unittest.TestCase):
    def test_exact_marker_and_tamper(self):
        project=Path(os.environ['PLANAR_EVALS_PROJECT_DIR'])
        base=project/'reviews'/'t046-schema2-native-proof-prep-20261117'
        output=base/'output'; records=output/'planar-account-admission-v1'/'records'
        rid='2b4416f4dda34ca39fc934a5762ad42b'; start=records/(rid+'.start.json'); marker=records/(rid+'.resolved.json')
        document=project/'reviews'/'t046-schema2-pretrial-release-approved-20261117.json'
        before=[hashlib.sha256(p.read_bytes()).hexdigest() for p in (marker,document)]
        self.assertEqual(before,['ff100dc4961fb8edf5546a73068fbe7667caf74c533f9ba18a299c7f03c164ec','673680e588eb9a4e2c327b9a118d70319cf9040bfaa15d5c6fa6603caf5ab178'])
        self.assertFalse(release.validate_published(start,marker))
        with (records.parent/'lock').open('r') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            token=codex_run._ACTIVE_ADMISSION.set(records.resolve())
            try:
                self.assertTrue(release.validate_published(start,marker))
                self.assertNotIn(start, codex_run.shared_pending(records))
                for env in ({'PLANAR_EVALS_PROJECT_DIR':''}, {'PLANAR_EVALS_LAYOUT_KIND':'new'}, {'PLANAR_EVALS_OUTPUT_ROOT':str(base/'wrong')}):
                    with patch.dict(os.environ,env):
                        self.assertFalse(release.validate_published(start,marker))
                original=release.read_safe
                def altered(path, root, *args, **kwargs):
                    value=original(path,root,*args,**kwargs)
                    return value+b' ' if Path(path)==document else value
                with patch.object(release,'read_safe',side_effect=altered):
                    self.assertFalse(release.validate_published(start,marker))
                with patch.object(release,'live_check',side_effect=ValueError('live resources')):
                    self.assertFalse(release.validate_published(start,marker))
            finally:
                codex_run._ACTIVE_ADMISSION.reset(token)
                fcntl.flock(lock,fcntl.LOCK_UN)
        self.assertEqual(before,[hashlib.sha256(p.read_bytes()).hexdigest() for p in (marker,document)])
