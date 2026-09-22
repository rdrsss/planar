#!/usr/bin/env python3
"""Record one planar-agent wrapper invocation as its own observed/ file.

Invoked by the `controlled-classic` `planar-agent` PATH wrapper after it
forwards a call to the real binary. Writes exactly one JSON document per
invocation to a uniquely-named file under the observed directory so that
up to sixteen concurrent wrapper lanes never interleave or truncate a
record: the document is built in a temp file under the observed directory
and moved into place with `os.replace`, which is an atomic rename on a
POSIX filesystem.

argv, and the captured stdout/stderr, are JSON-encoded by this module's
`json` import rather than shell-interpolated, so quotes, newlines, and
braces in a captured value round-trip exactly.

Usage (invoked by the bash wrapper, not by hand):
    record_observed.py <observed_dir> <seq> <ts> <exit_code> <json_flag> \
        <stdout_file> <stderr_file> -- <argv...>
"""
from __future__ import annotations

import json
import os
import sys
import tempfile


def main(argv: list[str]) -> int:
    if len(argv) < 9 or argv[8] != "--":
        sys.stderr.write(
            "record_observed.py: expected "
            "<observed_dir> <seq> <ts> <exit_code> <json_flag> "
            "<stdout_file> <stderr_file> -- <argv...>\n"
        )
        return 2
    observed_dir, seq, ts, exit_code, json_flag, stdout_file, stderr_file = argv[1:8]
    call_argv = argv[9:]

    with open(stdout_file, "r", encoding="utf-8", errors="replace") as handle:
        stdout_raw = handle.read()
    with open(stderr_file, "r", encoding="utf-8", errors="replace") as handle:
        stderr_raw = handle.read()

    stdout_value: object = stdout_raw
    if json_flag == "1":
        try:
            stdout_value = json.loads(stdout_raw)
        except json.JSONDecodeError:
            stdout_value = stdout_raw

    pid = os.getppid()
    record = {
        "ts": float(ts),
        "seq": int(seq),
        "pid": pid,
        "argv": call_argv,
        "exit_code": int(exit_code),
        "stdout": stdout_value,
        "stderr": stderr_raw,
    }

    os.makedirs(observed_dir, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(
        dir=observed_dir, prefix=".tmp-observed-", suffix=".json"
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as tmp:
            json.dump(record, tmp)
            tmp.write("\n")
        final_name = f"{ts}-{pid}-{seq}.json"
        os.replace(tmp_path, os.path.join(observed_dir, final_name))
    except BaseException:
        os.unlink(tmp_path)
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
