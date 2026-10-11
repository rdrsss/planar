#!/usr/bin/env python3
"""Tests for scripts/install-lib/queue_retire.py (plan 1089, task
qp-install-retire; tech spec 656, "Store reader commands"; test spec 658).

Run as ``python3 queue_retire.test.py [path/to/queue_retire.py]``; ctest runs
it as ``queue_retire_reader``. Every fixture is a scratch file under a
temporary directory, and every process it signals is a child this suite
started itself.

Rows that must count as live use a sleeping child of this process, with
``host_id`` and ``pid_started`` taken from the reader's OWN readers. That is
self-consistent on purpose: the post-build ctest case
``queue_retire_live_oracle`` is what checks those readers against the
engine's ``planar.process.identity``.
"""

import contextlib
import ctypes
import importlib.util
import io
import os
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
READER = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "queue_retire.py")
del sys.argv[1:]

_spec = importlib.util.spec_from_file_location("queue_retire", READER)
qr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qr)

DARWIN = sys.platform == "darwin"
LINUX = sys.platform.startswith("linux")

# The agent store's queue tables (migrations-agent 00002 + 00003).
AGENT_SCHEMA = """
create table agent_schema_migrations (version integer primary key, compat integer not null, description text not null);
create table queue_entries (
  seq integer primary key autoincrement,
  state text not null check(state in ('waiting', 'running')),
  host_id text not null, pid integer not null, pid_started integer not null,
  child_pgid integer, child_started integer, parent_seq integer,
  terminating_since_mono integer,
  terminate_reason text check(terminate_reason in ('timeout', 'cancelled')),
  cancelled_by text, cwd text not null, argv text not null, label text, vendor text,
  role text, claim_token text, log_path text, enqueued_at integer not null,
  started_at integer, refreshed_mono integer not null, deadline_mono integer,
  wait_deadline_mono integer, run_limit_ms integer, wait_limit_ms integer
);
create table queue_history (
  seq integer primary key, outcome text not null, exit_code integer, signal integer,
  successor_seq integer, cancelled_by text, nested integer not null default 0,
  parent_seq integer, cwd text not null, argv text not null, label text, vendor text,
  role text, log_path text, enqueued_at integer not null, started_at integer,
  ended_at integer not null, waited_ms integer not null, ran_ms integer,
  run_limit_ms integer, wait_limit_ms integer
);
"""


def make_store(path, entries=(), history=(), sequence=None):
    """Create an agent store at ``path`` holding ``entries`` (dicts) and
    ``history`` (seqs). ``sequence`` sets the sqlite_sequence row; None
    leaves whatever AUTOINCREMENT wrote (nothing when no entry has an
    explicit-less insert)."""
    conn = sqlite3.connect(path)
    conn.executescript(AGENT_SCHEMA)
    for row in entries:
        full = {
            "state": "waiting",
            "cwd": "/",
            "argv": '["sleep","300"]',
            "enqueued_at": 1,
            "refreshed_mono": 1,
            "child_pgid": None,
            "child_started": None,
        }
        full.update(row)
        cols = ", ".join(full)
        marks = ", ".join("?" for _ in full)
        conn.execute("insert into queue_entries ({}) values ({})".format(cols, marks), list(full.values()))
    for seq in history:
        conn.execute(
            "insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
            "values (?, 'exited', '/', '[]', 1, 2, 0)",
            (seq,),
        )
    if sequence is not None:
        conn.execute("delete from sqlite_sequence where name = 'queue_entries'")
        conn.execute("insert into sqlite_sequence (name, seq) values ('queue_entries', ?)", (sequence,))
    conn.commit()
    conn.close()


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def run_cli(*args, env=None):
    """Run the reader as a separate process, as install.sh does."""
    return subprocess.run([sys.executable, READER] + list(args), capture_output=True, text=True, env=env)


def run_main(*args):
    """Run the reader in this process (so a monkeypatched seam applies)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = qr.main(["queue_retire.py"] + list(args))
    return code, out.getvalue(), err.getvalue()


class Children:
    """Children started by this suite, killed and reaped on exit."""

    def __init__(self):
        self.procs = []

    def sleeper(self, new_group=False, argv0=None):
        cmd = ["sleep", "300"]
        if argv0 is not None:
            cmd = [argv0, "300"]
        proc = subprocess.Popen(cmd, start_new_session=new_group)
        self.procs.append(proc)
        return proc

    def reaped_pid(self):
        proc = subprocess.Popen(["true"])
        proc.wait()
        return proc.pid

    def close(self):
        for proc in self.procs:
            if proc.poll() is None:
                proc.kill()
            proc.wait()


def wait_for_start_time(pid):
    """The reader's start time for ``pid``; retried briefly after a spawn."""
    for _ in range(100):
        value = qr.start_time(pid)
        if value is not None:
            return value
        time.sleep(0.01)
    raise AssertionError("no start time for child {}".format(pid))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.kids = Children()
        self.host = qr.host_identity()
        self.assertIsNotNone(self.host, "this host's identity must be readable for these tests")

    def tearDown(self):
        self.kids.close()
        self.tmp.cleanup()

    def store(self, *entries, **kwargs):
        path = os.path.join(self.dir, "agent.db")
        if os.path.exists(path):
            os.remove(path)
        make_store(path, entries, **kwargs)
        return path

    def live_row(self, seq, proc, **extra):
        row = {"seq": seq, "host_id": self.host, "pid": proc.pid, "pid_started": wait_for_start_time(proc.pid)}
        row.update(extra)
        return row

    def verdicts(self, path):
        code, out, err = run_main("live", path)
        lines = {}
        for line in out.splitlines():
            kind, _, rest = line.partition(" ")
            if kind in ("blocking", "dead") and rest.startswith("seq="):
                lines[int(rest.split()[0][4:])] = kind
        return code, lines, out, err


class IdentityRules(unittest.TestCase):
    """Rules 1-3 of the per-row table (question 1010, corrected)."""

    LINUX_H = "11111111-1111-1111-1111-111111111111:pid:[4026531836]"

    def test_split(self):
        self.assertEqual(qr.split_identity("b:pid:[1]"), ("b", "pid:[1]"))
        self.assertEqual(qr.split_identity("ABC-DEF"), ("ABC-DEF", ""))

    def test_linux_table(self):
        h = self.LINUX_H
        cases = [
            ("22222222-2222-2222-2222-222222222222:pid:[4026531836]", "dead"),  # earlier boot / other machine
            ("11111111-1111-1111-1111-111111111111:pid:[4026532999]", "block"),  # a container: same boot, other ns
            ("22222222-2222-2222-2222-222222222222:pid:[4026532999]", "block"),  # both differ: the rest differs
            ("ABCDEF00-0000-0000-0000-000000000000", "block"),  # a macOS identity
            ("unknown", "block"),
            ("", "block"),
            (":pid:[4026531836]", "block"),  # an empty boot component: never written by the engine
            (h, "process"),
        ]
        for row_host, want in cases:
            with self.subTest(row_host=row_host):
                self.assertEqual(qr.judge_identity(h, row_host)[0], want)

    def test_macos_table(self):
        h = "ABCDEF00-0000-0000-0000-000000000000"
        self.assertEqual(qr.judge_identity(h, "12345678-0000-0000-0000-000000000000")[0], "dead")
        self.assertEqual(qr.judge_identity(h, self.LINUX_H)[0], "block")
        self.assertEqual(qr.judge_identity(h, h)[0], "process")
        self.assertEqual(qr.judge_identity(h, "unknown")[0], "block")

    def test_an_empty_boot_component_on_this_host_blocks(self):
        self.assertEqual(qr.judge_identity(":pid:[4026531836]", self.LINUX_H)[0], "block")

    def test_unknown_host_blocks_everything(self):
        self.assertEqual(qr.judge_identity(None, self.LINUX_H)[0], "block")
        self.assertTrue(qr.judge_row(None, {"host_id": self.LINUX_H, "state": "waiting", "pid": 1, "pid_started": 1})[0])


class Liveness(Base):
    """Rule 4: a row on this host's identity, judged by its processes."""

    def test_waiting_rows(self):
        live = self.kids.sleeper()
        reused = self.kids.sleeper()
        path = self.store(
            {"seq": 1, "host_id": self.host, "pid": self.kids.reaped_pid(), "pid_started": 5},
            self.live_row(2, live),
            {"seq": 3, "host_id": self.host, "pid": reused.pid, "pid_started": wait_for_start_time(reused.pid) + 1},
        )
        code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual(verdicts, {1: "dead", 2: "blocking", 3: "dead"}, out)
        self.assertEqual(code, qr.EXIT_BLOCKED)
        self.assertIn("1 blocking, 2 dead", out)

    def test_running_rows(self):
        group = self.kids.sleeper(new_group=True)
        reused_group = self.kids.sleeper(new_group=True)
        doomed = self.kids.sleeper(new_group=True)
        doomed_started = wait_for_start_time(doomed.pid)
        doomed.kill()
        doomed.wait()
        dead = self.kids.reaped_pid()
        path = self.store(
            {"seq": 1, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1,
             "child_pgid": group.pid, "child_started": wait_for_start_time(group.pid)},
            {"seq": 2, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1,
             "child_pgid": doomed.pid, "child_started": doomed_started},
            {"seq": 3, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1,
             "child_pgid": reused_group.pid, "child_started": wait_for_start_time(reused_group.pid) + 1},
            {"seq": 4, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1, "child_pgid": None},
            {"seq": 5, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1,
             "child_pgid": group.pid, "child_started": None},
        )
        code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual(verdicts, {1: "blocking", 2: "dead", 3: "dead", 4: "dead", 5: "blocking"}, out)
        self.assertEqual(code, qr.EXIT_BLOCKED)

    def test_only_dead_rows_exit_zero(self):
        path = self.store({"seq": 1, "host_id": self.host, "pid": self.kids.reaped_pid(), "pid_started": 5})
        code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual((code, verdicts), (qr.EXIT_OK, {1: "dead"}), out)

    def test_earlier_boot_row_is_dead_whatever_its_pid(self):
        boot, rest = qr.split_identity(self.host)
        other = "00000000-0000-0000-0000-000000000000" + (":" + rest if rest else "")
        live = self.kids.sleeper()
        path = self.store(self.live_row(1, live, host_id=other))
        code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual((code, verdicts), (qr.EXIT_OK, {1: "dead"}), out)
        self.assertIn("earlier boot", out)

    def test_eperm_on_the_submitter_is_live(self):
        row = {"host_id": self.host, "state": "waiting", "pid": 4242, "pid_started": 99}
        with mock.patch.object(qr.os, "kill", side_effect=PermissionError(1, "EPERM")), \
                mock.patch.object(qr, "start_time", return_value=99):
            self.assertTrue(qr.judge_row(self.host, row)[0])
        # EPERM only says the process exists: a provably different start time
        # still makes it a reused pid.
        with mock.patch.object(qr.os, "kill", side_effect=PermissionError(1, "EPERM")), \
                mock.patch.object(qr, "start_time", return_value=100):
            self.assertFalse(qr.judge_row(self.host, row)[0])

    def test_eperm_on_the_group_is_live(self):
        row = {"host_id": self.host, "state": "running", "pid": 4242, "pid_started": 99,
               "child_pgid": 4343, "child_started": 7}
        with mock.patch.object(qr.os, "kill", side_effect=ProcessLookupError(3, "ESRCH")), \
                mock.patch.object(qr.os, "killpg", side_effect=PermissionError(1, "EPERM")), \
                mock.patch.object(qr, "start_time", return_value=8):
            self.assertTrue(qr.judge_row(self.host, row)[0])

    def test_unexpected_kill_errors_are_live(self):
        row = {"host_id": self.host, "state": "running", "pid": 4242, "pid_started": 99,
               "child_pgid": 4343, "child_started": 7}
        with mock.patch.object(qr.os, "kill", side_effect=OSError(22, "EINVAL")):
            self.assertTrue(qr.judge_row(self.host, row)[0])
        with mock.patch.object(qr.os, "kill", side_effect=ProcessLookupError(3, "ESRCH")), \
                mock.patch.object(qr.os, "killpg", side_effect=OSError(22, "EINVAL")):
            self.assertTrue(qr.judge_row(self.host, row)[0])

    def test_unreadable_start_time_is_live(self):
        live = self.kids.sleeper()
        row = {"host_id": self.host, "state": "waiting", "pid": live.pid, "pid_started": 1}
        with mock.patch.object(qr, "start_time", return_value=None):
            self.assertTrue(qr.judge_row(self.host, row)[0])

    def test_stale_refresh_still_blocks(self):
        live = self.kids.sleeper()
        path = self.store(self.live_row(1, live, refreshed_mono=-10**12))
        _, verdicts, out, _ = self.verdicts(path)
        self.assertEqual(verdicts, {1: "blocking"}, out)

    def test_out_of_range_ids_block_and_are_never_signalled(self):
        dead = self.kids.reaped_pid()
        rows = [
            {"seq": 1, "host_id": self.host, "pid": 0, "pid_started": 1},
            {"seq": 2, "host_id": self.host, "pid": -5, "pid_started": 1},
            {"seq": 3, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1,
             "child_pgid": 1, "child_started": 1},
            {"seq": 4, "state": "running", "host_id": self.host, "pid": dead, "pid_started": 1,
             "child_pgid": 0, "child_started": 1},
        ]
        path = self.store(*rows)
        kills, killpgs = [], []
        real_kill, real_killpg = os.kill, os.killpg

        def spy_kill(pid, sig):
            kills.append((pid, sig))
            if pid <= 0:
                raise AssertionError("os.kill({}, ...) must never be called".format(pid))
            return real_kill(pid, sig)

        def spy_killpg(pgid, sig):
            killpgs.append((pgid, sig))
            if pgid <= 1:
                raise AssertionError("os.killpg({}, ...) must never be called".format(pgid))
            return real_killpg(pgid, sig)

        with mock.patch.object(qr.os, "kill", spy_kill), mock.patch.object(qr.os, "killpg", spy_killpg):
            code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual(verdicts, {1: "blocking", 2: "blocking", 3: "blocking", 4: "blocking"}, out)
        self.assertEqual(code, qr.EXIT_BLOCKED)
        self.assertTrue(all(pid > 0 for pid, _ in kills), kills)
        self.assertEqual(killpgs, [])
        self.assertTrue(all(sig == 0 for _, sig in kills))
        self.assertIn("cannot be proven dead", out)

    def test_unreadable_host_identity_blocks_every_row(self):
        live = self.kids.sleeper()
        rows = [self.live_row(1, live), {"seq": 2, "host_id": self.host, "pid": self.kids.reaped_pid(), "pid_started": 5}]
        path = self.store(*rows)
        if LINUX:
            seam = mock.patch.object(qr, "BOOT_ID_PATH", os.path.join(self.dir, "no-such-boot_id"))
        elif DARWIN:
            seam = mock.patch.object(qr, "_sysctlbyname", lambda name, buffer, size: -1)
        else:
            self.skipTest("no host identity source on this platform")
        with seam:
            self.assertIsNone(qr.host_identity())
            code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual(verdicts, {1: "blocking", 2: "blocking"}, out)
        self.assertEqual(code, qr.EXIT_BLOCKED)
        self.assertIn("identity cannot be read", out)

    def test_no_ps_on_path_changes_nothing(self):
        live = self.kids.sleeper()
        path = self.store(self.live_row(1, live), {"seq": 2, "host_id": self.host, "pid": self.kids.reaped_pid(), "pid_started": 5})
        with_path = run_cli("live", path)
        without = run_cli("live", path, env={"PATH": self.dir})
        self.assertEqual(with_path.returncode, qr.EXIT_BLOCKED, with_path.stderr)
        self.assertEqual((without.returncode, without.stdout), (with_path.returncode, with_path.stdout))


class FailClosed(Base):
    def test_an_unforeseen_exception_while_judging_exits_two(self):
        path = self.store({"seq": 1, "host_id": "unknown", "pid": 1, "pid_started": 1})
        with mock.patch.object(qr, "judge_row", side_effect=RuntimeError("judge exploded")):
            code, out, err = run_main("live", path)
        self.assertEqual(code, qr.EXIT_FAILED, out + err)
        self.assertIn("unexpected failure: judge exploded", err)
        self.assertEqual(len(err.strip().splitlines()), 1, err)

    def test_an_unforeseen_exception_reading_the_host_exits_two(self):
        path = self.store()
        with mock.patch.object(qr, "host_identity", side_effect=OSError(5, "EIO")):
            self.assertEqual(run_main("live", path)[0], qr.EXIT_FAILED)


class StartTimes(Base):
    def test_layout_is_pinned(self):
        self.assertEqual(qr.PROC_PIDTBSDINFO, 3)
        self.assertEqual(ctypes.sizeof(qr.ProcBsdInfo), 136)
        self.assertEqual(qr.ProcBsdInfo.pbi_start_tvsec.offset, 120)
        self.assertEqual(qr.ProcBsdInfo.pbi_start_tvusec.offset, 128)
        self.assertIsNone(qr.LAYOUT_PROBLEM)
        self.assertIsNone(qr.layout_problem())

    def test_a_wrong_layout_is_reported(self):
        class Short(ctypes.Structure):
            _fields_ = [("pad", ctypes.c_char * 112), ("pbi_start_tvsec", ctypes.c_uint64), ("pbi_start_tvusec", ctypes.c_uint64)]

        self.assertIn("size 128", qr.layout_problem(Short))
        self.assertIn("PROC_PIDTBSDINFO 4", qr.layout_problem(qr.ProcBsdInfo, 4))

    @unittest.skipUnless(DARWIN, "macOS start-time source")
    def test_a_wrong_struct_makes_every_existing_pid_live(self):
        class Short(ctypes.Structure):
            _fields_ = [("pad", ctypes.c_char * 112), ("pbi_start_tvsec", ctypes.c_uint64), ("pbi_start_tvusec", ctypes.c_uint64)]

        live = self.kids.sleeper()
        truth = wait_for_start_time(live.pid)
        path = self.store({"seq": 1, "host_id": self.host, "pid": live.pid, "pid_started": truth + 1})
        self.assertEqual(self.verdicts(path)[1], {1: "dead"})
        with mock.patch.object(qr, "BSDINFO", Short):
            self.assertIsNone(qr.start_time(live.pid))
            code, verdicts, out, _ = self.verdicts(path)
        self.assertEqual((code, verdicts), (qr.EXIT_BLOCKED, {1: "blocking"}), out)
        self.assertIn("layout mismatch", out)

    def test_parse_after_the_last_paren(self):
        fields = " ".join(str(n) for n in range(3, 30))  # field 3 .. 29; field 22 is "22"
        line = "123 (a) (b c) {}\n".format(fields).encode()
        self.assertEqual(qr.parse_linux_stat(line), 22)
        self.assertEqual(qr.parse_linux_stat(b"123 (sleep) " + fields.encode()), 22)

    def test_garbled_stat_lines_parse_to_nothing(self):
        for bad in (b"", b"123 sleep S 1 2", b"123 (x) S 1 2 3", b"123 (x) " + b" ".join([b"1"] * 19) + b" +5",
                    b"123 (x) " + b" ".join([b"1"] * 19) + b" 5_0", b"123 (x) " + b" ".join([b"1"] * 19) + b" " + str(2**64).encode(), None):
            with self.subTest(bad=bad):
                self.assertIsNone(qr.parse_linux_stat(bad))

    @unittest.skipUnless(LINUX, "/proc start times")
    def test_odd_comm_child(self):
        link = os.path.join(self.dir, "a) (b c")
        os.symlink("/bin/sleep" if os.path.exists("/bin/sleep") else "/usr/bin/sleep", link)
        child = self.kids.sleeper(argv0=link)
        truth = wait_for_start_time(child.pid)
        with open("/proc/{}/stat".format(child.pid), "rb") as handle:
            self.assertIn(b") (", handle.read())
        path = self.store({"seq": 1, "host_id": self.host, "pid": child.pid, "pid_started": truth},
                          {"seq": 2, "host_id": self.host, "pid": child.pid, "pid_started": truth + 1})
        self.assertEqual(self.verdicts(path)[1], {1: "blocking", 2: "dead"})

    @unittest.skipUnless(LINUX, "/proc start times")
    def test_garbled_or_missing_proc_entry_is_live(self):
        live = self.kids.sleeper()
        row = {"host_id": self.host, "state": "waiting", "pid": live.pid, "pid_started": 1}
        with mock.patch.object(qr, "_read_proc_stat", return_value=b"garbled"):
            self.assertTrue(qr.judge_row(self.host, row)[0])
        with mock.patch.object(qr, "_read_proc_stat", side_effect=FileNotFoundError(2, "gone")):
            self.assertTrue(qr.judge_row(self.host, row)[0])


class Store(Base):
    def assert_read_failure(self, *args):
        result = run_cli(*args)
        self.assertEqual(result.returncode, qr.EXIT_FAILED, result.stdout + result.stderr)
        self.assertEqual(len(result.stderr.strip().splitlines()), 1, result.stderr)
        self.assertIn("by hand", result.stderr)
        return result

    def test_unreadable_stores_fail_closed(self):
        text = os.path.join(self.dir, "text.db")
        with open(text, "w") as handle:
            handle.write("not a database\n")
        bare = os.path.join(self.dir, "bare.db")
        conn = sqlite3.connect(bare)
        conn.execute("create table other (x)")
        conn.close()
        no_history = os.path.join(self.dir, "nohistory.db")
        conn = sqlite3.connect(no_history)
        conn.execute("create table queue_entries (seq integer primary key autoincrement, state text)")
        conn.close()
        for path in (text, bare, no_history, os.path.join(self.dir, "missing.db")):
            for command in ("live", "oldmax"):
                with self.subTest(path=os.path.basename(path), command=command):
                    self.assert_read_failure(command, path)

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root reads a mode-000 file")
    def test_mode_000_fails_closed(self):
        path = self.store()
        os.chmod(path, 0)
        try:
            self.assert_read_failure("live", path)
            self.assert_read_failure("oldmax", path)
        finally:
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)

    def test_oldmax(self):
        path = self.store()
        self.assertEqual(run_cli("oldmax", path).stdout.strip(), "0")
        path = self.store({"seq": 12, "host_id": "unknown", "pid": 1, "pid_started": 1}, history=(57,), sequence=40)
        self.assertEqual(run_cli("oldmax", path).stdout.strip(), "57")
        path = self.store({"seq": 12, "host_id": "unknown", "pid": 1, "pid_started": 1}, history=(57,), sequence=99)
        self.assertEqual(run_cli("oldmax", path).stdout.strip(), "99")
        path = self.store(sequence=1000000)
        self.assertEqual(run_cli("oldmax", path).stdout.strip(), "1000000")

    def test_rows_without_a_sequence_row_fail_closed(self):
        path = self.store(history=(3,))
        self.assert_read_failure("oldmax", path)

    def test_awkward_path_reaches_python_intact_and_opens_read_only(self):
        awkward = os.path.join(self.dir, "a b?c'd#%")
        os.mkdir(awkward)
        path = os.path.join(awkward, "agent.db")
        make_store(path, [{"seq": 5, "host_id": "unknown", "pid": 1, "pid_started": 1}])
        # A decoy where an unencoded URI would land.
        make_store(os.path.join(self.dir, "a b"), [])
        before = read_bytes(path)
        os.chmod(path, stat.S_IRUSR)
        try:
            live = run_cli("live", path)
            self.assertEqual(live.returncode, qr.EXIT_BLOCKED, live.stderr)
            self.assertIn("blocking seq=5 ", live.stdout)
            self.assertEqual(run_cli("oldmax", path).stdout.strip(), "5")
        finally:
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
        self.assertEqual(read_bytes(path), before)


if __name__ == "__main__":
    unittest.main(verbosity=1)
