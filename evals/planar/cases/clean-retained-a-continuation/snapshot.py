"""Bounded host-side selection of a Docker archive of this case's repository.

This parser is not a process census, an injection hook, or a verifier handoff.
Callers must obtain the archive directly from the identified stopped container.
"""
import hashlib
import io
import re
import tarfile
import zlib

MAX_ARCHIVE = 8 * 1024 * 1024
MAX_FILE = 4 * 1024 * 1024
MAX_ENTRIES = 128
TOP = {'README.md', 'RESULT.txt', 'STATE.txt'}
GIT_FILES = {'.git/HEAD', '.git/index', '.git/config', '.git/description',
             '.git/info/exclude', '.git/logs/HEAD', '.git/packed-refs'}
GIT_DIRS = {'.git', '.git/branches', '.git/hooks', '.git/info', '.git/logs',
            '.git/objects', '.git/objects/info', '.git/objects/pack', '.git/refs',
            '.git/refs/heads', '.git/refs/tags', '.git/objects/e6'}
EMPTY_BLOB = '.git/objects/e6/9de29bb2d1d6434b8b29ae775ad8c2e48c5391'


def _git_file(name):
    return (name in GIT_FILES or name == EMPTY_BLOB or
            re.fullmatch(r'\.git/hooks/[a-z0-9-]+\.sample', name) is not None or
            re.fullmatch(r'\.git/objects/pack/pack-[0-9a-f]{40}\.(?:pack|idx|rev)', name) is not None)


def _diagnostic(name):
    # Never echo arbitrary archive strings, control characters or long paths.
    return name if re.fullmatch(r'[A-Za-z0-9_./-]{1,160}',name) else 'sha256:'+hashlib.sha256(name.encode('utf-8','surrogateescape')).hexdigest()


def select_repository(data):
    if not isinstance(data, bytes) or len(data) > MAX_ARCHIVE:
        raise ValueError('invalid archive size')
    selected, inventory, directories, seen = {}, {}, set(), set()
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for item in archive:
            name = item.name
            while name.startswith('./'): name = name[2:]
            if name in ('', '.') and item.isdir(): continue
            if (not name or name.startswith('/') or any(x in ('', '.', '..') for x in name.split('/'))
                or name in seen or len(seen) >= MAX_ENTRIES):
                raise ValueError('unsafe or repeated repository entry')
            seen.add(name)
            if item.isdir():
                if name not in GIT_DIRS: raise ValueError('unexpected repository directory: '+_diagnostic(name))
                directories.add(name)
                continue
            if not item.isfile() or item.size > MAX_FILE or not (name in TOP or _git_file(name)):
                raise ValueError('unexpected repository entry: '+_diagnostic(name))
            reader = archive.extractfile(item)
            if reader is None: raise ValueError('unreadable repository entry')
            content = reader.read(MAX_FILE+1)
            if len(content) != item.size: raise ValueError('invalid repository entry length')
            if name == EMPTY_BLOB:
                if len(content)>128: raise ValueError('invalid empty Git blob')
                try:
                    decoder=zlib.decompressobj(); body=decoder.decompress(content,16)
                    if body!=b'blob 0\0' or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail: raise ValueError('invalid empty Git blob')
                except zlib.error: raise ValueError('invalid empty Git blob') from None
            # Hash every permitted metadata file; retain bytes only for exact
            # candidate paths and the two Git files needed by the verifier.
            inventory[name] = {'size': len(content), 'sha256': hashlib.sha256(content).hexdigest()}
            if name in TOP or name in ('.git/HEAD', '.git/index'):
                selected['repository/'+name] = content
    if not {'README.md', '.git/HEAD', '.git/index'} <= inventory.keys():
        raise ValueError('required repository evidence missing')
    return selected, {'files':inventory, 'directories':sorted(directories)}
