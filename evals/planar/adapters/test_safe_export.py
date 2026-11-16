import io
import tarfile
import tempfile
import unittest
from pathlib import Path

from safe_export import select_archive, write_selected, MAX_FILE


def make(name, data=b'2\n', kind=tarfile.REGTYPE):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w') as archive:
        info = tarfile.TarInfo(name)
        info.type = kind
        if kind == tarfile.SYMTYPE:
            info.linkname = '/etc/passwd'
        else:
            info.size = len(data)
        archive.addfile(info, io.BytesIO(data) if info.isfile() else None)
    return out.getvalue()


class ExportTests(unittest.TestCase):
    def test_allowed_regular_file_only(self):
        self.assertEqual(select_archive(make('RESULT.txt'), {'RESULT.txt'}), {'RESULT.txt': b'2\n'})
        for archive in (make('../RESULT.txt'), make('/RESULT.txt'), make('RESULT.txt', kind=tarfile.SYMTYPE), make('secret.txt'), make('RESULT.txt', b'x' * (MAX_FILE + 1))):
            with self.assertRaises(ValueError):
                select_archive(archive, {'RESULT.txt'})

    def test_output_refuses_symlink_and_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'RESULT.txt').symlink_to('/etc/passwd')
            with self.assertRaises(OSError):
                write_selected(root, {'RESULT.txt': b'2\n'})
            with self.assertRaises(ValueError):
                write_selected(root, {'../RESULT.txt': b'2\n'})

    def test_native_rollout_allowlist_and_symlink_rejection(self):
        from safe_export import select_native_logs
        path = 'sessions/2026/11/16/rollout-2026-test.jsonl'
        self.assertEqual(select_native_logs(make(path, b'{}\n')), {path: b'{}\n'})
        with self.assertRaises(ValueError):
            select_native_logs(make(path, kind=tarfile.SYMTYPE))
        with self.assertRaises(ValueError):
            select_native_logs(make('sessions/2026/11/16/../auth.json'))

    def test_harbor_setup_directory_and_verifier_flat_exports(self):
        from safe_export import select_flat_archive, select_native_logs
        out = io.BytesIO()
        with tarfile.open(fileobj=out, mode='w') as archive:
            directory = tarfile.TarInfo('./setup'); directory.type = tarfile.DIRTYPE
            archive.addfile(directory)
            log = tarfile.TarInfo('./codex.txt'); log.size = 3
            archive.addfile(log, io.BytesIO(b'{}\n'))
        self.assertEqual(select_native_logs(out.getvalue()), {'codex.txt': b'{}\n'})
        self.assertEqual(select_flat_archive(make('reward.txt', b'0\n'), {'reward.txt', 'status.json', 'test-stdout.txt'}), {'reward.txt': b'0\n'})
        with self.assertRaises(ValueError):
            select_native_logs(make('setup/secret.txt'))
        with self.assertRaises(ValueError):
            select_flat_archive(make('status.json', kind=tarfile.SYMTYPE), {'status.json'})
        with self.assertRaises(ValueError):
            select_flat_archive(make('../status.json'), {'status.json'})
