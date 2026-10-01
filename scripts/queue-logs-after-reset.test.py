#!/usr/bin/env python3
"""Runner for scripts/queue-logs-after-reset.py (plan 1089, task qp-migration).

Registered as the ctest case `queue_logs_after_reset_helper` in
src/cmd/planar/CMakeLists.txt. It builds every fixture in a scratch
directory, runs the helper with `python3`, and asserts on the exit code,
stdout, stderr and the resulting tree (test-spec scenario "Edge -- the
counter-reset helper uses the restored sequence and archives instead of
deleting").

Usage: queue-logs-after-reset.test.py <path to queue-logs-after-reset.py>
"""

import datetime
import hashlib
import os
import re
import sqlite3
import stat
import subprocess
import sys
import tempfile
import traceback

HELPER = None
FAILURES = []


def run(*args):
    """Run the helper with `args`; return (exit code, stdout, stderr)."""
    proc = subprocess.run(
        [sys.executable, HELPER, *args],
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout, proc.stderr


def check(cond, what):
    """Record a failed assertion without stopping the case."""
    if not cond:
        FAILURES.append(what)
        print(f"FAIL: {what}", file=sys.stderr)


def digest(path):
    """SHA-256 of a file's bytes."""
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def tree(root):
    """Every file under `root`, as sorted paths relative to it."""
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            out.append(os.path.relpath(os.path.join(dirpath, name), root))
    return sorted(out)


def touch_logs(log_dir, names):
    """Create `log_dir` and one small file per name in it."""
    os.makedirs(log_dir, exist_ok=True)
    for name in names:
        with open(os.path.join(log_dir, name), "w", encoding="utf-8") as handle:
            handle.write(f"output of {name}\n")


def make_restored_db(path, sequence, history_max):
    """A database shaped like a restored planar.db: queue tables, a
    sqlite_sequence row for queue_entries at `sequence` (None: no row), and
    pruned history whose highest seq is `history_max`."""
    conn = sqlite3.connect(path)
    conn.execute("create table queue_entries (seq integer primary key autoincrement, state text)")
    conn.execute("create table queue_history (seq integer primary key, outcome text)")
    conn.execute("create table plans (id integer primary key autoincrement, title text)")
    conn.execute("insert into plans (title) values ('p')")
    if sequence is not None:
        conn.execute("insert into sqlite_sequence (name, seq) values ('queue_entries', ?)", (sequence,))
    if history_max is not None:
        conn.execute("insert into queue_history (seq, outcome) values (?, 'exited')", (history_max,))
    conn.commit()
    conn.close()


def listed_logs(stdout):
    """The log file names the helper reported, in order."""
    return [os.path.basename(match) for match in re.findall(r"(\S+/\d+\.log)\b", stdout)]


def archives_in(log_dir):
    """The reset-archive-* directories in `log_dir`, sorted."""
    return sorted(name for name in os.listdir(log_dir) if name.startswith("reset-archive-"))


def case_threshold_and_archive(scratch):
    """The restored sequence, not max(seq), is the threshold; --apply
    archives (mode 0700), deletes nothing, and a second run finds nothing."""
    db = os.path.join(scratch, "planar.db")
    make_restored_db(db, 1000050, 1000020)
    log_dir = os.path.join(scratch, "queue-logs")
    names = ["1000010.log", "1000030.log", "1000050.log", "1000051.log", "1000099.log"]
    touch_logs(log_dir, names + ["notes.txt", "abc.log"])
    before_hash = digest(db)
    before_tree = tree(scratch)

    code, out, err = run(db)
    check(code == 0, f"dry run exits 0 (got {code}, stderr {err!r})")
    check(listed_logs(out) == ["1000051.log", "1000099.log"], f"dry run lists exactly the two logs above 1000050: {out!r}")
    check("1000050" in out, f"dry run names the threshold: {out!r}")
    check(tree(scratch) == before_tree, "dry run changes nothing")

    code, out, err = run("--apply", db)
    check(code == 0, f"--apply exits 0 (got {code}, stderr {err!r})")
    archives = archives_in(log_dir)
    check(len(archives) == 1, f"--apply creates exactly one archive directory: {archives}")
    if archives:
        archive = os.path.join(log_dir, archives[0])
        check(re.fullmatch(r"reset-archive-\d{8}T\d{6}Z", archives[0]) is not None, f"archive name is a UTC stamp: {archives[0]}")
        check(stat.S_IMODE(os.stat(archive).st_mode) == 0o700, f"archive is mode 0700: {oct(os.stat(archive).st_mode)}")
        check(sorted(os.listdir(archive)) == ["1000051.log", "1000099.log"], f"archive holds the two logs: {os.listdir(archive)}")
        check(archive in out, f"--apply prints the archive path: {out!r}")
    for kept in ["1000010.log", "1000030.log", "1000050.log", "notes.txt", "abc.log"]:
        check(os.path.isfile(os.path.join(log_dir, kept)), f"{kept} stays where it was")
    for moved in ["1000051.log", "1000099.log"]:
        check(not os.path.exists(os.path.join(log_dir, moved)), f"{moved} left the log namespace")
    check(len(tree(scratch)) == len(before_tree), "no file was deleted: the file count is unchanged")

    code, out, err = run("--apply", db)
    check(code == 0, f"a second --apply exits 0 (got {code}, stderr {err!r})")
    check(listed_logs(out) == [], f"a second --apply finds nothing to move: {out!r}")
    check(len(archives_in(log_dir)) == 1, "a second --apply creates no new archive")
    check(len(tree(scratch)) == len(before_tree), "the second run deletes nothing either")

    check(digest(db) == before_hash, "the database is opened read-only: its content hash is unchanged")


def case_floor_and_stem(scratch):
    """No queue_entries row in sqlite_sequence: threshold 1000000; a
    database not named planar.db uses <stem>.queue-logs/."""
    db = os.path.join(scratch, "other.db")
    make_restored_db(db, None, None)
    log_dir = os.path.join(scratch, "other.queue-logs")
    touch_logs(log_dir, ["999999.log", "1000000.log", "1000001.log"])
    # A planar.db-style directory beside it must be ignored.
    touch_logs(os.path.join(scratch, "queue-logs"), ["1000005.log"])

    code, out, err = run(db)
    check(code == 0, f"floor dry run exits 0 (got {code}, stderr {err!r})")
    check(listed_logs(out) == ["1000001.log"], f"threshold is 1000000 without a queue_entries row: {out!r}")
    check("other.queue-logs" in out, f"the helper works in other.queue-logs/: {out!r}")

    code, out, err = run("--apply", db)
    check(code == 0, f"floor --apply exits 0 (got {code}, stderr {err!r})")
    archives = archives_in(log_dir)
    check(len(archives) == 1 and os.listdir(os.path.join(log_dir, archives[0])) == ["1000001.log"], f"archived 1000001.log only: {archives}")
    check(os.path.isfile(os.path.join(scratch, "queue-logs", "1000005.log")), "the other database's directory is untouched")


def case_archive_collision(scratch):
    """A name that already exists in the archive directory refuses that file
    and leaves it in place; the other file still moves."""
    db = os.path.join(scratch, "planar.db")
    make_restored_db(db, 1000000, None)
    log_dir = os.path.join(scratch, "queue-logs")
    touch_logs(log_dir, ["1000001.log", "1000002.log"])
    # Pre-create the archive directory for every second the run could start
    # in, each already holding a 1000001.log.
    now = datetime.datetime.now(datetime.timezone.utc)
    for offset in range(0, 30):
        stamp = (now + datetime.timedelta(seconds=offset)).strftime("%Y%m%dT%H%M%SZ")
        touch_logs(os.path.join(log_dir, f"reset-archive-{stamp}"), ["1000001.log"])
    before = len(tree(scratch))

    code, out, err = run("--apply", db)
    check(code != 0, f"a refused file makes the run exit non-zero (got {code})")
    check("1000001.log" in err, f"the refusal names the file: {err!r}")
    check(os.path.isfile(os.path.join(log_dir, "1000001.log")), "the refused file stays in place")
    check(not os.path.exists(os.path.join(log_dir, "1000002.log")), "the other file still moves")
    check(len(tree(scratch)) == before, "nothing is deleted or overwritten")


def case_no_log_directory(scratch):
    """A valid planar.db with no queue-logs/ directory at all (a host that
    never ran a detached run): both modes exit 0, say there is nothing to
    archive, and create nothing."""
    db = os.path.join(scratch, "planar.db")
    make_restored_db(db, 1000050, 1000020)
    check(not os.path.exists(os.path.join(scratch, "queue-logs")), "fixture: no log directory")
    before = tree(scratch)
    before_entries = sorted(os.listdir(scratch))
    for args in ([db], ["--apply", db]):
        code, out, err = run(*args)
        check(code == 0, f"no log directory: {args} exits 0 (got {code}, stderr {err!r})")
        check("nothing to archive" in out, f"no log directory: {args} says there is nothing to archive: {out!r}")
        check(listed_logs(out) == [], f"no log directory: {args} lists nothing: {out!r}")
        check(not os.path.exists(os.path.join(scratch, "queue-logs")), f"no log directory: {args} creates no log directory")
        check(tree(scratch) == before and sorted(os.listdir(scratch)) == before_entries, f"no log directory: {args} changes nothing")


def case_path_quoting(scratch):
    """A database path with a space, '?', '#' and '%' still opens the right
    file: the read-only URI percent-encodes the path."""
    odd = os.path.join(scratch, "we ird?x#y%41")
    os.makedirs(odd)
    db = os.path.join(odd, "planar.db")
    make_restored_db(db, 1000070, None)
    # A decoy the unencoded URI would resolve to ("we ird" with the query cut off).
    make_restored_db(os.path.join(scratch, "we ird"), 1000005, None)
    log_dir = os.path.join(odd, "queue-logs")
    touch_logs(log_dir, ["1000070.log", "1000071.log"])
    before_hash = digest(db)

    code, out, err = run(db)
    check(code == 0, f"quoted path: dry run exits 0 (got {code}, stderr {err!r})")
    check("threshold: 1000070" in out, f"quoted path: the threshold comes from that database: {out!r}")
    check(listed_logs(out) == ["1000071.log"], f"quoted path: lists only 1000071.log: {out!r}")

    code, out, err = run("--apply", db)
    check(code == 0, f"quoted path: --apply exits 0 (got {code}, stderr {err!r})")
    archives = archives_in(log_dir)
    check(len(archives) == 1 and os.listdir(os.path.join(log_dir, archives[0])) == ["1000071.log"], f"quoted path: archived 1000071.log: {archives}")
    check(os.path.isfile(os.path.join(log_dir, "1000070.log")), "quoted path: 1000070.log stays")
    check(digest(db) == before_hash, "quoted path: the database is unchanged")


def fail_closed(scratch, label, db, setup=None):
    """Assert the helper refuses `db` with exactly one stderr line and moves
    nothing from its log directory."""
    log_dir = os.path.join(os.path.dirname(db), os.path.splitext(os.path.basename(db))[0] + ".queue-logs")
    touch_logs(log_dir, ["1000001.log", "2000000.log"])
    if setup:
        setup()
    before = tree(scratch)
    for args in ([db], ["--apply", db]):
        code, out, err = run(*args)
        check(code != 0, f"{label}: {args} exits non-zero (got {code}, stdout {out!r})")
        lines = [line for line in err.splitlines() if line.strip()]
        check(len(lines) == 1, f"{label}: exactly one line names the cause: {err!r}")
        check(tree(scratch) == before, f"{label}: nothing moved")


def case_fail_closed(scratch):
    """No sqlite_sequence table, a text file, an unreadable file."""
    no_seq = os.path.join(scratch, "noseq.db")
    conn = sqlite3.connect(no_seq)
    conn.execute("create table plain (id integer primary key, v text)")
    conn.execute("insert into plain (v) values ('x')")
    conn.commit()
    conn.close()
    fail_closed(scratch, "no sqlite_sequence table", no_seq)
    code, _out, err = run(no_seq)
    check("has no sqlite_sequence table" in err, f"the refusal says the sqlite_sequence table is missing: {err!r}")

    text = os.path.join(scratch, "text.db")
    with open(text, "w", encoding="utf-8") as handle:
        handle.write("this is not a database\n" * 100)
    fail_closed(scratch, "text file", text)

    locked = os.path.join(scratch, "locked.db")
    make_restored_db(locked, 1000000, None)
    if os.geteuid() == 0:
        # root reads a mode-000 file, so the case cannot be built here.
        print("note: running as root; the chmod 000 case is not observable", file=sys.stderr)
    else:
        fail_closed(scratch, "chmod 000 file", locked, setup=lambda: os.chmod(locked, 0))
        os.chmod(locked, 0o600)

    missing = os.path.join(scratch, "missing.db")
    fail_closed(scratch, "missing file", missing)


def main():
    global HELPER
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    HELPER = sys.argv[1]
    cases = [
        case_threshold_and_archive,
        case_floor_and_stem,
        case_archive_collision,
        case_fail_closed,
        case_no_log_directory,
        case_path_quoting,
    ]
    for case in cases:
        with tempfile.TemporaryDirectory(prefix="qlar-") as scratch:
            print(f"== {case.__name__}")
            try:
                case(scratch)
            except Exception:  # noqa: BLE001 -- any crash is a failed case
                FAILURES.append(f"{case.__name__} raised")
                traceback.print_exc()
            finally:
                for dirpath, dirs, files in os.walk(scratch):
                    for name in dirs + files:
                        try:
                            os.chmod(os.path.join(dirpath, name), 0o700)
                        except OSError:
                            pass
    if FAILURES:
        print(f"{len(FAILURES)} assertion(s) failed", file=sys.stderr)
        return 1
    print(f"all {len(cases)} cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
