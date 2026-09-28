#!/usr/bin/env python3
"""Detect a damaged ninja dependency log, and remove it when asked.

Ninja records each object's header dependencies in <build-dir>/.ninja_deps.
Two ninja processes building the same directory at once can interleave their
records. Ninja then stops reading at the first bad record on every later run,
warns "premature end of file; recovering", and discards everything recorded
after that point, so every object looks stale and every build is a full
rebuild. Nothing else reports this.

Usage: ninja-deps-check.py [--repair] <build-dir>

Exit 0 when the log is intact or absent, or was removed by --repair.
Exit 1 when the log is damaged and --repair was not given.
"""
import struct
import sys
from pathlib import Path


def first_bad_offset(data: bytes):
    """Return (offset, reason) for the first unreadable record, or None."""
    offset, paths = 16, 0
    while offset < len(data):
        if offset + 4 > len(data):
            return offset, "truncated record header"
        (size,) = struct.unpack("<I", data[offset:offset + 4])
        is_deps, size = size >> 31, size & 0x7FFFFFFF
        if offset + 4 + size > len(data):
            return offset, "record runs past the end of the file"
        if not is_deps:
            (checksum,) = struct.unpack("<I", data[offset + size:offset + 4 + size])
            found = ~checksum & 0xFFFFFFFF
            if found != paths:
                return offset, f"path numbered {found} where {paths} was expected"
            paths += 1
        offset += 4 + size
    return None


def main(argv):
    repair = "--repair" in argv
    args = [a for a in argv if a != "--repair"]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 64
    log = Path(args[0]) / ".ninja_deps"
    if not log.is_file():
        return 0
    data = log.read_bytes()
    if not data.startswith(b"# ninjadeps\n"):
        bad = (0, "missing header")
    else:
        bad = first_bad_offset(data)
    if bad is None:
        return 0
    offset, reason = bad
    lost = 100 * (len(data) - offset) // max(len(data), 1)
    print(f"ninja-deps-check: {log} is damaged at byte {offset}: {reason}; "
          f"{lost}% of the log is unreadable", file=sys.stderr)
    if not repair:
        print("ninja-deps-check: every build in this directory is a full rebuild "
              "until the log is removed; rerun with --repair", file=sys.stderr)
        return 1
    log.unlink()
    print("ninja-deps-check: removed it; the next build recompiles everything once",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
