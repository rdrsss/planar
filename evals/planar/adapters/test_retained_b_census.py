import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from retained_b_census import check

class Census(TestCase):
    def test_clear_and_unsafe_candidates(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp); own=os.getpid(); uid=os.getuid(); output=f'{uid} {own} 1 python3\n0 1 0 launchd\n{uid} 999999 1 python3\n'
            def run(*a,**kw): return SimpleNamespace(returncode=0,stdout=output)
            self.assertEqual(check(root,'planar-t044-unique',root,run=run,argv_reader=lambda p:['python','other.py'])['matching_commands'],0)
            for argv in (['python',str(root/'evals/planar/adapters/watchdog.py')],['sh','-c','planar-t044-unique']):
                with self.subTest(argv=argv),self.assertRaises(ValueError): check(root,'planar-t044-unique',root,run=run,argv_reader=lambda p:argv)
            def unavailable(_): raise OSError('race')
            with self.assertRaisesRegex(ValueError,'unavailable'): check(root,'planar-t044-unique',root,run=run,argv_reader=unavailable)
