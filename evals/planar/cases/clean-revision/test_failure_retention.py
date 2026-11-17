"""Rejected home archives remain bounded, private, unextracted evidence."""
import asyncio
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
import capture


def forbidden_archive():
    data=io.BytesIO()
    with tarfile.open(fileobj=data,mode='w') as tar:
        info=tarfile.TarInfo('projects/revision-fault/secret-token.md')
        raw=b'synthetic-sensitive-data'; info.size=len(raw); tar.addfile(info,io.BytesIO(raw))
    return data.getvalue()

class RetentionTests(unittest.TestCase):
    def test_selector_rejection_retains_exact_archive_without_extracting(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); data=forbidden_archive()
            env=type('Environment',(),{'_archive':AsyncMock(return_value=data)})()
            git=['08bacc341a31f32c14a1695b5b2de5c44b3a3e56\n','stage','status']
            with patch.object(capture,'census',return_value=['baseline']), patch.object(capture,'_git',side_effect=git):
                with self.assertRaisesRegex(ValueError,'<redacted>'):
                    asyncio.run(capture.collect(env,'a'*64,['baseline'],failure_evidence=root))
            archive=root/'rejected-home.tar'; receipt=root/'rejected-home.json'
            self.assertEqual(archive.read_bytes(),data)
            self.assertEqual(stat.S_IMODE(archive.stat().st_mode),0o600)
            self.assertEqual(stat.S_IMODE(receipt.stat().st_mode),0o600)
            info=json.loads(receipt.read_text())
            self.assertEqual(info,{'reason':'bounded_home_selector_rejected','source':'/workspace/home/.','size':len(data),'sha256':hashlib.sha256(data).hexdigest()})
            self.assertEqual({p.name for p in root.iterdir()},{'rejected-home.tar','rejected-home.json'})
            with self.assertRaises(FileExistsError): capture.retain_rejected_home(b'other',root)
            self.assertEqual(archive.read_bytes(),data)

    def test_oversize_and_unsafe_destination_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.assertRaisesRegex(ValueError,'size'): capture.retain_rejected_home(b'x'*(capture.MAX_ARCHIVE+1),root)
            self.assertEqual(list(root.iterdir()),[])
            link=root/'link'; link.symlink_to(root,target_is_directory=True)
            with self.assertRaisesRegex(ValueError,'unsafe'): capture.retain_rejected_home(b'x',link)
            self.assertFalse((root/'rejected-home.tar').exists())

if __name__=='__main__': unittest.main()
