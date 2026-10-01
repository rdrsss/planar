#!/usr/bin/env python3
"""queue-logs-after-reset.py -- archive detached queue logs a counter reset
would collide with (plan 1089, decision 1222; tech spec 656 § Counter resets).

Usage:
    python3 scripts/queue-logs-after-reset.py [--apply] <planar.db>

A rollback of migration 00040 (down, then up), a backup restore or a re-init
returns `queue_entries`' AUTOINCREMENT counter to an earlier value. Detached
`planar-agent queue run --detach` logs are created with O_EXCL, so the next
detached run whose sequence number names an existing `<seq>.log` refuses at
exit 125 rather than overwrite it. That log may be the only remaining record
of a run whose history row the reset discarded, so nothing replaces it
automatically. This helper is the documented recipe instead
(migrations/README.md § Host-queue rollback recovery).

The threshold is the value AUTOINCREMENT continues from: the
`sqlite_sequence` row for `queue_entries`, or 1000000 (the migration's floor)
when that row is absent. It is not `max(seq)` of the queue tables, because
history is pruned after `[queue] history_days`, and a log between the pruned
maximum and the counter can never collide.

The database is opened read-only (`mode=ro`), so the helper never changes
it. It fails closed, exiting 1 with one line on stderr, when the file cannot
be opened or read, is not a SQLite database, or has no `sqlite_sequence`
table at all (a real planar.db always has one; its first migration creates
AUTOINCREMENT tables).

The log directory is the database's own: `<dir>/queue-logs/` for a file named
`planar.db`, `<dir>/<stem>.queue-logs/` for any other name (`stem` is the
name with its final extension removed).

Without `--apply` the helper lists the logs numbered above the threshold and
changes nothing. With `--apply` it ARCHIVES them -- it never deletes -- by
renaming each into `<log-dir>/reset-archive-<UTC yyyymmddThhmmssZ>/`
(mode 0700) and prints the archive path. Archived names no longer sit in the
`<seq>.log` namespace, so they cannot collide, and queue retention never
prunes them: delete an archive by hand once it is no longer needed. A name
that already exists in the archive refuses that one file, leaves it in
place, and makes the run exit 1.

Exit codes: 0 success (including nothing to do), 1 a refusal or a failure,
2 a usage error.
"""

import datetime
import os
import re
import sqlite3
import sys
import urllib.parse

PROG = "queue-logs-after-reset"
FLOOR = 1000000
LOG_NAME = re.compile(r"^([0-9]+)\.log$")


def fail(message):
    """Print one line naming the failure and exit 1."""
    print(f"{PROG}: {message}", file=sys.stderr)
    sys.exit(1)


def log_directory(db_path):
    """The database's detached-log directory (tech spec 656 § Store and open
    path): `queue-logs/` beside `planar.db`, `<stem>.queue-logs/` otherwise."""
    directory, name = os.path.split(db_path)
    if name == "planar.db":
        return os.path.join(directory, "queue-logs")
    stem, _ext = os.path.splitext(name)
    return os.path.join(directory, f"{stem}.queue-logs")


def read_threshold(db_path):
    """The value AUTOINCREMENT continues from, read through a read-only
    connection; exits through `fail` when it cannot be read."""
    if not os.path.isfile(db_path):
        fail(f"{db_path} is not a file; pass the path of the planar.db whose counter was reset")
    uri = "file:" + urllib.parse.quote(db_path) + "?mode=ro"
    try:
        conn = sqlite3.connect(uri, uri=True)
    except sqlite3.Error as err:
        fail(f"cannot open {db_path} read-only: {err}; check the path and its permissions")
    try:
        has_sequence = conn.execute(
            "select count(*) from sqlite_master where type = 'table' and name = 'sqlite_sequence'"
        ).fetchone()[0]
        if has_sequence == 0:
            fail(
                f"{db_path} has no sqlite_sequence table, so it is not a planar.db and has no queue counter; "
                "pass the planar.db whose counter was reset"
            )
        row = conn.execute("select seq from sqlite_sequence where name = 'queue_entries'").fetchone()
    except sqlite3.Error as err:
        fail(f"cannot read {db_path} as a SQLite database: {err}; pass the planar.db whose counter was reset")
    finally:
        conn.close()
    if row is None or row[0] is None:
        return FLOOR, "no sqlite_sequence row for queue_entries; the migration floor"
    try:
        return int(row[0]), "the sqlite_sequence row for queue_entries"
    except (TypeError, ValueError):
        fail(f"{db_path}'s sqlite_sequence row for queue_entries is not an integer: {row[0]!r}")
    return None  # unreachable: fail() exits


def logs_above(log_dir, threshold):
    """`(seq, path)` of every `<seq>.log` file in `log_dir` numbered above
    `threshold`, lowest first."""
    found = []
    try:
        names = os.listdir(log_dir)
    except FileNotFoundError:
        return found
    except OSError as err:
        fail(f"cannot list {log_dir}: {err}")
    for name in names:
        match = LOG_NAME.match(name)
        path = os.path.join(log_dir, name)
        if match and os.path.isfile(path) and not os.path.islink(path) and int(match.group(1)) > threshold:
            found.append((int(match.group(1)), path))
    return sorted(found)


def archive(log_dir, logs):
    """Move `logs` into a new reset-archive directory; return the number of
    files refused because their name already exists there."""
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    target = os.path.join(log_dir, f"reset-archive-{stamp}")
    try:
        os.mkdir(target, 0o700)
        os.chmod(target, 0o700)
    except FileExistsError:
        if not os.path.isdir(target) or os.path.islink(target):
            fail(f"{target} exists and is not a directory; move it aside and re-run")
    except OSError as err:
        fail(f"cannot create {target}: {err}")
    refused = 0
    for _seq, path in logs:
        destination = os.path.join(target, os.path.basename(path))
        if os.path.lexists(destination):
            print(f"{PROG}: {destination} already exists; {path} was left in place", file=sys.stderr)
            refused += 1
            continue
        try:
            os.rename(path, destination)
        except OSError as err:
            print(f"{PROG}: cannot move {path} to {destination}: {err}; it was left in place", file=sys.stderr)
            refused += 1
            continue
        print(f"archived {path} -> {destination}")
    print(f"archive: {target}")
    return refused


def main(argv):
    args = argv[1:]
    apply = False
    if args and args[0] == "--apply":
        apply = True
        args = args[1:]
    if len(args) != 1 or args[0].startswith("-"):
        print(f"usage: python3 scripts/{PROG}.py [--apply] <planar.db>", file=sys.stderr)
        return 2
    db_path = os.path.abspath(args[0])

    threshold, source = read_threshold(db_path)
    log_dir = log_directory(db_path)
    logs = logs_above(log_dir, threshold)

    print(f"threshold: {threshold} ({source})")
    print(f"log directory: {log_dir}")
    if not logs:
        print("nothing to archive: no log is numbered above the threshold")
        return 0
    if not apply:
        for _seq, path in logs:
            print(f"would archive {path}")
        print(f"{len(logs)} log(s) would collide with new runs; re-run with --apply to archive them")
        return 0
    return 1 if archive(log_dir, logs) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
