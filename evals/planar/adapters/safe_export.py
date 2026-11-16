"""Strict in-memory tar inspection for a future bounded Docker export path.

This is not integrated with Harbor's artifact/log collector. Do not run a
credentialed job until the actual collector is replaced or independently proven.
"""
from __future__ import annotations

import io
import os
from pathlib import Path, PurePosixPath
import tarfile

MAX_ARCHIVE = 8 * 1024 * 1024
MAX_FILE = 4 * 1024 * 1024
MAX_FILES = 64


def select_archive(data: bytes, allowed: set[str]) -> dict[str, bytes]:
    if len(data) > MAX_ARCHIVE:
        raise ValueError('archive exceeds bound')
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for member in archive:
            name = member.name
            path = PurePosixPath(name)
            if not name or path.is_absolute() or any(p in ('.', '..') for p in name.split('/')):
                raise ValueError('unsafe archive path')
            if member.isdir():
                if not any(item.startswith(name.rstrip('/') + '/') for item in allowed):
                    raise ValueError('unexpected archive directory')
                continue
            if not member.isfile() or name not in allowed or name in result:
                raise ValueError('unexpected, repeated or non-regular archive entry')
            if member.size > MAX_FILE or len(result) >= MAX_FILES:
                raise ValueError('archive entry exceeds bound')
            reader = archive.extractfile(member)
            if reader is None:
                raise ValueError('unreadable entry')
            content = reader.read(MAX_FILE + 1)
            if len(content) != member.size:
                raise ValueError('invalid entry length')
            result[name] = content
    return result


def write_selected(destination: Path, selected: dict[str, bytes]) -> None:
    if destination.is_symlink() or not destination.is_dir():
        raise ValueError('unsafe destination')
    # No subdirectories: callers map approved archive names to explicit leaf
    # output names before writing, so no intermediate symlink can be followed.
    for name, content in selected.items():
        if '/' in name or name in ('', '.', '..'):
            raise ValueError('output must be an approved leaf name')
        fd = os.open(destination / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'wb') as output:
            output.write(content)


def select_native_logs(data: bytes) -> dict[str, bytes]:
    """Allow only Codex's output and dated native rollout files."""
    import re
    if len(data) > MAX_ARCHIVE:
        raise ValueError('archive exceeds bound')
    allowed = set()
    ignored = set()
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for member in archive:
            name = member.name
            while name.startswith('./'):
                name = name[2:]
            if name in ('', '.') and member.isdir():
                continue
            if name != member.name:
                # Docker commonly emits ./ prefixes; normalize only those.
                member.name = name
            if member.isdir():
                if name != 'setup' and not re.fullmatch(r'sessions(?:/\d{4}(?:/\d{2}(?:/\d{2})?)?)?', name):
                    raise ValueError('unexpected native log directory')
                continue
            # Harbor uploads its locally generated trajectory back into the
            # agent logs before artifact collection. Do not export that derived
            # file; accept only one bounded regular entry at this exact path.
            if name == 'trajectory.json':
                if not member.isfile() or name in ignored or member.size > MAX_FILE:
                    raise ValueError('invalid derived trajectory entry')
                ignored.add(name)
                continue
            if name != 'codex.txt' and not re.fullmatch(r'sessions/\d{4}/\d{2}/\d{2}/rollout-[A-Za-z0-9_.-]+\.jsonl', name):
                raise ValueError('unexpected native log file')
            if not member.isfile() or name in allowed or member.size > MAX_FILE or len(allowed) >= MAX_FILES:
                raise ValueError('invalid native log entry')
            allowed.add(name)
    # Repack only after full validation into memory, avoiding any untrusted
    # extraction on disk. The original bytes are still bounded in memory.
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for member in archive:
            name = member.name
            while name.startswith('./'):
                name = name[2:]
            if name not in allowed or member.isdir():
                continue
            reader = archive.extractfile(member)
            if reader is None:
                raise ValueError('unreadable native log')
            content = reader.read(MAX_FILE + 1)
            if len(content) != member.size:
                raise ValueError('invalid native log length')
            result[name] = content
    return result


def write_tree(destination: Path, selected: dict[str, bytes]) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    root_fd = os.open(destination, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for name, content in selected.items():
            parts = name.split('/')
            if any(part in ('', '.', '..') for part in parts):
                raise ValueError('unsafe output path')
            current = os.dup(root_fd)
            try:
                for part in parts[:-1]:
                    try:
                        os.mkdir(part, 0o700, dir_fd=current)
                    except FileExistsError:
                        pass
                    child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=current)
                    os.close(current)
                    current = child
                fd = os.open(parts[-1], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=current)
                with os.fdopen(fd, 'wb') as output:
                    output.write(content)
            finally:
                os.close(current)
    finally:
        os.close(root_fd)


def select_flat_archive(data: bytes, allowed: set[str]) -> dict[str, bytes]:
    """Validate a Docker directory archive containing only named regular files."""
    if len(data) > MAX_ARCHIVE:
        raise ValueError('archive exceeds bound')
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for member in archive:
            name = member.name
            while name.startswith('./'):
                name = name[2:]
            if name in ('', '.') and member.isdir():
                continue
            if name not in allowed or not member.isfile() or name in result or member.size > MAX_FILE or len(result) >= MAX_FILES:
                raise ValueError('unexpected flat archive entry')
            reader = archive.extractfile(member)
            if reader is None:
                raise ValueError('unreadable flat archive entry')
            content = reader.read(MAX_FILE + 1)
            if len(content) != member.size:
                raise ValueError('invalid flat archive length')
            result[name] = content
    return result
