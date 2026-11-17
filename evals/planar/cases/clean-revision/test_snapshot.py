import io
import tarfile
import unittest
from snapshot import select_repository


def archive(entries):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w') as tar:
        for name, content in entries:
            info = tarfile.TarInfo(name)
            if content is None:
                info.type = tarfile.SYMTYPE; info.linkname = 'other'; tar.addfile(info)
            else:
                info.size = len(content); tar.addfile(info, io.BytesIO(content))
    return output.getvalue()

class SnapshotTests(unittest.TestCase):
    def test_select_and_hash_unexported_git_config(self):
        entries = [('./README.md', b'base'), ('./.git/HEAD', b'head'), ('./.git/index', b'index'), ('./.git/config', b'config'), ('./STATE.txt', b'correct\n')]
        selected, meta = select_repository(archive(entries))
        self.assertEqual(selected['repository/STATE.txt'], b'correct\n')
        self.assertNotIn('repository/.git/config', selected)
        self.assertIn('.git/config', meta['files'])
    def test_unexpected_duplicate_and_symlink_fail(self):
        base = [('README.md', b'x'), ('.git/HEAD', b'x'), ('.git/index', b'x')]
        for extra in ([('../escape', b'x')], [('unplanned', b'x')], [('STATE.txt',None)], [('README.md',b'x')], [('.git/hooks/evil',b'x')]):
            with self.subTest(extra=extra), self.assertRaises(ValueError): select_repository(archive(base+extra))
    def test_missing_index_fails(self):
        with self.assertRaisesRegex(ValueError, 'required'): select_repository(archive([('README.md',b'x'),('.git/HEAD',b'x')]))

if __name__ == '__main__': unittest.main()
