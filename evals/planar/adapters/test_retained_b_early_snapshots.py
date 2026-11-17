import shutil
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
import launch
from retained_b_early_snapshots import inspect

SOURCE=launch.PROJECT/'reviews/t045-retained-b-final-prep-20261117/output/jobs/planar-t044-aeaec469c5c549ee96f3ce7729dc4c2e/retained-b'

class EarlySnapshots(TestCase):
    def setUp(self):
        if not SOURCE.is_dir(): self.skipTest('retained snapshots unavailable')
        self.tmp=TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root=Path(self.tmp.name)
        for stage in ('restored-pre','post','final'): shutil.copytree(SOURCE/stage,self.root/stage)
        shutil.copyfile(SOURCE/'injection.json',self.root/'injection.json')
    def test_original(self): self.assertTrue(inspect(self.root)['post_final_equal'])
    def test_changed_missing_extra_and_link_fail(self):
        p=self.root/'final/repository/STATE.txt'; original=p.read_bytes()
        for mode in ('changed','missing','extra','link'):
            if mode=='changed': p.write_bytes(b'drift')
            if mode=='missing': p.unlink()
            if mode=='extra': (self.root/'final/extra').write_text('extra')
            if mode=='link': p.unlink(); p.symlink_to(self.root/'post/repository/STATE.txt')
            with self.subTest(mode=mode),self.assertRaises(ValueError): inspect(self.root)
            if p.is_symlink(): p.unlink()
            p.write_bytes(original); (self.root/'final/extra').unlink(missing_ok=True)
