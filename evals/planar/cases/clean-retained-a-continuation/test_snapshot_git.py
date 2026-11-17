import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
import zlib
from install_fixture import install
from snapshot import EMPTY_BLOB, select_repository


def archive(root):
    data=io.BytesIO()
    with tarfile.open(fileobj=data,mode='w') as tar:
        for path in sorted(root.rglob('*')): tar.add(path,arcname=path.relative_to(root).as_posix(),recursive=False)
    return data.getvalue()


def mutated(raw,kind):
    out=io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(raw)) as original, tarfile.open(fileobj=out,mode='w') as target:
        for item in original:
            stream=original.extractfile(item) if item.isfile() else None; data=stream.read() if stream else None
            if item.name==EMPTY_BLOB:
                if kind=='bad-data': data=zlib.compress(b'blob 1\0x'); item.size=len(data)
                elif kind=='symlink': item.type=tarfile.SYMTYPE;item.linkname='elsewhere';item.size=0;data=None
                elif kind=='oversize': data=b'x'*129;item.size=len(data)
            target.addfile(item,io.BytesIO(data) if data is not None else None)
            if kind=='duplicate' and item.name==EMPTY_BLOB: target.addfile(item,io.BytesIO(data))
        if kind=='unknown-dir':
            item=tarfile.TarInfo('.git/objects/aa');item.type=tarfile.DIRTYPE;target.addfile(item)
    return out.getvalue()


class IntentToAdd(unittest.TestCase):
    def test_real_git_intent_to_add_and_bounded_negatives(self):
        with tempfile.TemporaryDirectory() as tmp:
            workspace=Path(tmp);install(workspace); repo=workspace/'repository'
            (repo/'RESULT.txt').write_bytes(b'2\n');(repo/'STATE.txt').write_bytes(b'correct\n')
            subprocess.run(['git','add','-N','--','RESULT.txt','STATE.txt'],cwd=repo,check=True,capture_output=True)
            self.assertTrue((repo/EMPTY_BLOB).is_file()); raw=archive(repo)
            selected,inventory=select_repository(raw)
            self.assertEqual(selected['repository/RESULT.txt'],b'2\n');self.assertIn(EMPTY_BLOB,inventory['files'])
            for kind in ('bad-data','symlink','oversize','duplicate','unknown-dir'):
                with self.subTest(kind=kind),self.assertRaises(ValueError): select_repository(mutated(raw,kind))

    def test_unsafe_offender_is_not_echoed(self):
        from snapshot import _diagnostic
        self.assertTrue(_diagnostic('unsafe\npath').startswith('sha256:'))
        self.assertTrue(_diagnostic('x'*161).startswith('sha256:'))
