#!/usr/bin/env python3
"""queue_retire.py -- install.sh's reader for the queue upgrade (plan 1089).

install.sh sources ``scripts/install-lib/queue-retire.sh`` and runs this file
as ``python3 queue_retire.py <command> ...`` (tech spec 656, "Install and
upgrade"; decisions 1219-1229; questions 1006-1011). Every path arrives
through ``sys.argv``; none is ever interpolated into source. A store is
opened read-only, as ``file:`` + the percent-encoded absolute path +
``?mode=ro``, so a ``?``, ``#`` or ``%`` in it cannot change which file is
opened.

Commands:

``probe-verdict <exit-code>``
    Classifies one ``planar-agent queue status 1 --json`` probe of the
    prefix ``planar.db``. The probe's stdout arrives on stdin and its exit
    code as the argument. Prints one line, ``<verdict>\\t<detail>``, where
    the verdict is ``usable``, ``behind``, ``incompatible``, ``foreign`` or
    ``failed``. Anything the probe table does not name, including output
    that is not JSON, is ``failed``.

``live <agent.db>``
    Judges every ``queue_entries`` row of the retired store the way the
    engine's liveness does (``src/engine/hostqueue/liveness.cpp``), plus the
    reboot rule of question 1010, and prints each row with its verdict and
    the rule that decided it. Exits 0 when no row blocks, 3 when one does,
    and 2 when the store cannot be read.

``oldmax <agent.db>``
    Prints the store's old maximum sequence number: the larger of
    ``max(seq)`` over both queue tables and the ``sqlite_sequence`` row for
    ``queue_entries``; 0 when both tables are empty and there is no such
    row. Exits 2 when the store cannot be read, or has rows but no
    ``sqlite_sequence`` row.

Fail-closed is the rule throughout. A store that cannot be read exits 2. A
row that cannot be PROVEN dead blocks: an unknown host identity, another pid
namespace, an id the engine would not test, a ``kill`` that fails with
``EPERM`` or any unexpected error, and a start time that cannot be read are
all "live". The reader is stricter than the engine and both differences can
only block: it applies no freshness test (it shares no monotonic clock with
the writer), and it refuses to call a row with an out-of-range pid or pgid
dead where the engine would call it "no process".

It never calls ``os.kill`` with a pid at or below 0 (``kill(0, ...)`` is the
reader's own group) or ``os.killpg`` with a pgid at or below 1
(``killpg(1, ...)`` addresses every process), and the only signal it sends is
0.

The module uses only the Python standard library, ``ctypes`` included. It
shells out to no program: there is no ``ps`` and no ``sysctl`` (question
1011), so it adds no installer dependency besides ``python3``.
"""

import ctypes
import ctypes.util
import json
import os
import re
import sqlite3
import sys
import urllib.parse

# Exit codes of this script.
EXIT_OK = 0
EXIT_FAILED = 2  # the store could not be read, or the script was misused
EXIT_BLOCKED = 3  # `live`: at least one row blocks

# The literal the engine stores when it cannot read the host identity
# (`k_unknown_host_identity`, src/lib/process/identity.cppm).
UNKNOWN_HOST = "unknown"

# The largest value a platform pid_t holds; the engine's `is_pid` refuses
# anything above it (src/lib/process/identity.cpp).
_PID_MAX = 2**31 - 1

# Start times are compared as the engine compares them: as uint64.
_U64 = 2**64

# The remedy every read failure names.
_READ_REMEDY = (
    "if no old `planar-agent queue` command is still running, delete agent.db, "
    "agent.db-wal, agent.db-shm and the old numbered logs in queue-logs/ by hand, "
    "then re-run ./install.sh"
)


class StoreError(Exception):
    """The store could not be read. Its message is one line."""


# ---------------------------------------------------------------------------
# probe-verdict

# `planar-agent` exit codes the probe table reads (tech spec 656, "Store and
# open path"): 125 for every queue refusal, 1 for `not_found` and for a parse
# failure, 2 for `invalid_input`.
_EXIT_QUEUE_REFUSED = 125
_EXIT_NOT_FOUND = 1

# The 125 tags that do not abort the install, and the verdict each maps to.
_REFUSAL_VERDICTS = {
    "schema_version_behind": "behind",
    "queue_schema_incompatible": "incompatible",
    "queue_schema_foreign": "foreign",
}


def _one_line(text):
    """Collapse ``text`` onto one line so a verdict or a row stays one line."""
    return " ".join(str(text).split())


def classify_probe(exit_code, stdout_text):
    """Classify one probe from its exit code and its stdout.

    Returns ``(verdict, detail)``. The mapping is the probe table of tech
    spec 656, "Steps, in order", step 3: exit 0 with a status object, or exit
    1 with tag ``not_found``, is usable; exit 125 with one of the three tags
    in ``_REFUSAL_VERDICTS`` is that verdict; everything else is ``failed``.
    """
    try:
        document = json.loads(stdout_text)
    except ValueError:
        document = None
    tag = None
    if isinstance(document, dict):
        error = document.get("error")
        if isinstance(error, dict) and isinstance(error.get("tag"), str):
            tag = error["tag"]

    if exit_code == 0 and isinstance(document, dict) and "error" not in document:
        return "usable", "exit 0 with a status object"
    if exit_code == _EXIT_NOT_FOUND and tag == "not_found":
        return "usable", "exit 1, tag not_found"
    if exit_code == _EXIT_QUEUE_REFUSED and tag in _REFUSAL_VERDICTS:
        return _REFUSAL_VERDICTS[tag], "exit 125, tag " + tag

    if document is None:
        shape = "output that is not JSON"
    elif tag is None:
        shape = "JSON with no error tag"
    else:
        shape = "tag " + tag
    return "failed", "exit {} with {}".format(exit_code, shape)


# ---------------------------------------------------------------------------
# Host identity, read exactly as `native_identity_source` builds it
# (src/lib/process/identity.cpp).

# Linux sources. Module attributes so a test can point them elsewhere.
BOOT_ID_PATH = "/proc/sys/kernel/random/boot_id"
PID_NS_LINK = "/proc/self/ns/pid"


def _libc():
    path = ctypes.util.find_library("c")
    return ctypes.CDLL(path, use_errno=True)


def _sysctlbyname(name, buffer, size):
    """``sysctlbyname(name, buffer, &size, NULL, 0)`` from libc.

    A module-level function so a test can substitute a failing one.
    """
    fn = _libc().sysctlbyname
    fn.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t]
    fn.restype = ctypes.c_int
    return fn(name, buffer, ctypes.byref(size), None, 0)


def _linux_identity():
    try:
        with open(BOOT_ID_PATH, "rb") as handle:
            boot = handle.read()
        pid_ns = os.readlink(PID_NS_LINK)
    except OSError:
        return None
    # `read_small_file` strips trailing \n, \r and spaces, one at a time.
    try:
        boot = boot.rstrip(b"\n\r ").decode("utf-8")
    except UnicodeDecodeError:
        return None
    if not boot or not pid_ns:
        return None
    return boot + ":" + pid_ns


def _macos_identity():
    try:
        buffer = ctypes.create_string_buffer(128)
        size = ctypes.c_size_t(len(buffer))
        if _sysctlbyname(b"kern.bootsessionuuid", buffer, size) != 0 or size.value == 0:
            return None
        # The engine keeps `size` bytes and strips trailing \0 and \n.
        value = buffer.raw[: size.value].rstrip(b"\0\n").decode("utf-8")
    except (OSError, AttributeError, ValueError, UnicodeDecodeError):
        return None
    return value or None


def host_identity():
    """This host's identity, or None when it cannot be read."""
    if sys.platform == "darwin":
        return _macos_identity()
    if sys.platform.startswith("linux"):
        return _linux_identity()
    return None


def split_identity(identity):
    """Split an identity into its boot component and the rest.

    Linux identities are ``<boot id>:<pid-namespace link>``; the boot id is a
    UUID, with no ``:``, so the boot component is the text before the first
    ``:``. A macOS identity has no ``:``; all of it is the boot component and
    the rest is empty.
    """
    boot, sep, rest = identity.partition(":")
    return boot, rest if sep else ""


def judge_identity(host, row_host):
    """Decide what the identities alone say about a row.

    Returns ``(verdict, rule)`` where the verdict is ``block``, ``dead`` or
    ``process`` (same identity: judge the row by its processes).
    """
    if host is None:
        return "block", "this host's identity cannot be read"
    if not isinstance(row_host, str) or not row_host or row_host == UNKNOWN_HOST:
        return "block", "the row's host identity is unknown"
    if row_host == host:
        return "process", "same host"
    host_boot, host_rest = split_identity(host)
    row_boot, row_rest = split_identity(row_host)
    if host_boot != row_boot and host_rest == row_rest:
        return "dead", "an earlier boot of this machine, or another machine"
    return "block", "another pid namespace (a container on this kernel, or a foreign identity)"


# ---------------------------------------------------------------------------
# Start times, read in the unit the engine stores.

# macOS `struct proc_bsdinfo` (<sys/proc_info.h>). Every field is
# fixed-width, so arm64 and x86_64 share this layout. The four values below
# are pinned and checked at import (tech spec 656, "Start times").
PROC_PIDTBSDINFO = 3
_PINNED_SIZE = 136
_PINNED_TVSEC = 120
_PINNED_TVUSEC = 128
_PINNED_FLAVOR = 3


class ProcBsdInfo(ctypes.Structure):
    _fields_ = [
        ("pbi_flags", ctypes.c_uint32),
        ("pbi_status", ctypes.c_uint32),
        ("pbi_xstatus", ctypes.c_uint32),
        ("pbi_pid", ctypes.c_uint32),
        ("pbi_ppid", ctypes.c_uint32),
        ("pbi_uid", ctypes.c_uint32),
        ("pbi_gid", ctypes.c_uint32),
        ("pbi_ruid", ctypes.c_uint32),
        ("pbi_rgid", ctypes.c_uint32),
        ("pbi_svuid", ctypes.c_uint32),
        ("pbi_svgid", ctypes.c_uint32),
        ("rfu_1", ctypes.c_uint32),
        ("pbi_comm", ctypes.c_char * 16),
        ("pbi_name", ctypes.c_char * 32),
        ("pbi_nfiles", ctypes.c_uint32),
        ("pbi_pgid", ctypes.c_uint32),
        ("pbi_pjobc", ctypes.c_uint32),
        ("e_tdev", ctypes.c_uint32),
        ("e_tpgid", ctypes.c_uint32),
        ("pbi_nice", ctypes.c_int32),
        ("pbi_start_tvsec", ctypes.c_uint64),
        ("pbi_start_tvusec", ctypes.c_uint64),
    ]


# The struct the macOS reader uses. A module attribute so a test can
# substitute a wrong one and see the reader refuse to trust it.
BSDINFO = ProcBsdInfo


def layout_problem(struct=None, flavor=None):
    """Describe a mismatch with the pinned ``proc_bsdinfo`` layout, or None."""
    struct = BSDINFO if struct is None else struct
    flavor = PROC_PIDTBSDINFO if flavor is None else flavor
    problems = []
    try:
        size = ctypes.sizeof(struct)
        tvsec = struct.pbi_start_tvsec.offset
        tvusec = struct.pbi_start_tvusec.offset
    except (AttributeError, TypeError):
        return "proc_bsdinfo has no pbi_start_tvsec/pbi_start_tvusec fields"
    if size != _PINNED_SIZE:
        problems.append("size {} (pinned {})".format(size, _PINNED_SIZE))
    if tvsec != _PINNED_TVSEC:
        problems.append("pbi_start_tvsec at {} (pinned {})".format(tvsec, _PINNED_TVSEC))
    if tvusec != _PINNED_TVUSEC:
        problems.append("pbi_start_tvusec at {} (pinned {})".format(tvusec, _PINNED_TVUSEC))
    if flavor != _PINNED_FLAVOR:
        problems.append("PROC_PIDTBSDINFO {} (pinned {})".format(flavor, _PINNED_FLAVOR))
    if problems:
        return "proc_bsdinfo layout mismatch: " + ", ".join(problems)
    return None


# Asserted at import. A mismatch does not raise: it makes the macOS
# start-time source unusable, so no start time is ever "provably different"
# and every row on this boot whose pid still exists blocks.
LAYOUT_PROBLEM = layout_problem()


def _macos_start_time(pid):
    if LAYOUT_PROBLEM is not None or layout_problem() is not None:
        return None
    try:
        path = ctypes.util.find_library("proc")
        libproc = ctypes.CDLL(path, use_errno=True)
        fn = libproc.proc_pidinfo
        fn.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
        fn.restype = ctypes.c_int
        info = BSDINFO()
        got = fn(pid, PROC_PIDTBSDINFO, 0, ctypes.byref(info), ctypes.sizeof(info))
    except (OSError, AttributeError, TypeError, ValueError):
        return None
    if got != ctypes.sizeof(info):
        return None
    # identity.cpp: seconds * 1'000'000 + microseconds, in uint64.
    return (info.pbi_start_tvsec * 1_000_000 + info.pbi_start_tvusec) % _U64


_DIGITS = re.compile(rb"[0-9]+\Z")


def parse_linux_stat(data):
    """Field 22 (``starttime``) of a ``/proc/<pid>/stat`` line, or None.

    The parse starts after the LAST ``)``, as ``parse_starttime`` does, so a
    command name holding spaces or parentheses cannot shift the fields.
    ``starttime`` is index 19 of the whitespace split of what follows. Any
    parse failure is None, which the callers treat as "not provably
    different".
    """
    if not isinstance(data, (bytes, bytearray)):
        return None
    close = data.rfind(b")")
    if close < 0:
        return None
    fields = data[close + 1 :].split()
    if len(fields) <= 19 or not _DIGITS.match(fields[19]):
        return None
    value = int(fields[19])
    if value >= _U64:
        return None
    return value


def _read_proc_stat(pid):
    """The bytes of ``/proc/<pid>/stat``. A test seam."""
    with open("/proc/{}/stat".format(pid), "rb") as handle:
        return handle.read()


def _linux_start_time(pid):
    try:
        data = _read_proc_stat(pid)
    except OSError:
        return None
    return parse_linux_stat(data)


def start_time(pid):
    """The start time of ``pid`` in the engine's unit, or None when it cannot
    be read for any reason."""
    if sys.platform == "darwin":
        return _macos_start_time(pid)
    if sys.platform.startswith("linux"):
        return _linux_start_time(pid)
    return None


def _same_start(read, stored):
    return read == stored % _U64


# ---------------------------------------------------------------------------
# Per-row liveness


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def submitter_dead(pid, pid_started):
    """``(dead, reason)`` for a row's submitter. ``dead`` is True only when
    the submitter is provably gone."""
    if not _is_int(pid) or pid <= 0 or pid > _PID_MAX:
        return False, "pid {!r} is not a process id; cannot be proven dead".format(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True, "submitter pid {} is gone".format(pid)
    except PermissionError:
        pass  # it exists; another user's process
    except OSError as err:
        return False, "kill({}, 0) failed ({}); cannot be proven dead".format(pid, err.strerror)
    if not _is_int(pid_started):
        return False, "submitter pid {} exists and pid_started is not a number".format(pid)
    started = start_time(pid)
    if started is None:
        return False, "submitter pid {} exists and its start time cannot be read".format(pid)
    if not _same_start(started, pid_started):
        return True, "submitter pid {} was reused (start time {} != {})".format(pid, started, pid_started)
    return False, "submitter pid {} is alive".format(pid)


def group_dead(pgid, child_started):
    """``(dead, reason)`` for a running row's child group."""
    if not _is_int(pgid) or pgid <= 1 or pgid > _PID_MAX:
        return False, "child_pgid {!r} is not a process group; cannot be proven dead".format(pgid)
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return True, "child group {} is gone".format(pgid)
    except PermissionError:
        return False, "child group {} exists (EPERM)".format(pgid)
    except OSError as err:
        return False, "killpg({}, 0) failed ({}); cannot be proven dead".format(pgid, err.strerror)
    if child_started is None:
        return False, "child group {} has members".format(pgid)
    if not _is_int(child_started):
        return False, "child group {} has members and child_started is not a number".format(pgid)
    leader = start_time(pgid)
    if leader is not None and not _same_start(leader, child_started):
        return True, "child group leader {} was reused (start time {} != {})".format(pgid, leader, child_started)
    return False, "child group {} has members".format(pgid)


def judge_row(host, row):
    """``(blocks, rule)`` for one ``queue_entries`` row (a dict)."""
    verdict, rule = judge_identity(host, row.get("host_id"))
    if verdict == "block":
        return True, rule
    if verdict == "dead":
        return False, rule

    state = row.get("state")
    if state not in ("waiting", "running"):
        return True, "unrecognised state {!r}; cannot be proven dead".format(state)
    dead, why = submitter_dead(row.get("pid"), row.get("pid_started"))
    if not dead:
        return True, why
    if state == "running" and row.get("child_pgid") is not None:
        gone, group_why = group_dead(row.get("child_pgid"), row.get("child_started"))
        if not gone:
            return True, why + "; " + group_why
        return False, why + "; " + group_why
    return False, why


# ---------------------------------------------------------------------------
# The store


def open_store(path):
    """Open ``path`` read-only. Raises StoreError."""
    abs_path = os.path.abspath(path)
    uri = "file:" + urllib.parse.quote(abs_path) + "?mode=ro"
    try:
        conn = sqlite3.connect(uri, uri=True)
        for table in ("queue_entries", "queue_history"):
            found = conn.execute("select 1 from sqlite_master where type = 'table' and name = ?", (table,)).fetchone()
            if found is None:
                raise StoreError("{} has no {} table".format(abs_path, table))
    except sqlite3.Error as err:
        raise StoreError("cannot read {}: {}".format(abs_path, _one_line(err)))
    return conn


_ROW_COLUMNS = ("seq", "state", "host_id", "pid", "pid_started", "child_pgid", "child_started", "argv")


def read_rows(conn, path):
    try:
        cursor = conn.execute("select {} from queue_entries order by seq".format(", ".join(_ROW_COLUMNS)))
        return [dict(zip(_ROW_COLUMNS, values)) for values in cursor.fetchall()]
    except sqlite3.Error as err:
        raise StoreError("cannot read the queue entries of {}: {}".format(os.path.abspath(path), _one_line(err)))


def old_max(conn, path):
    """The old maximum sequence number. Raises StoreError."""
    abs_path = os.path.abspath(path)
    try:
        values = []
        for table in ("queue_entries", "queue_history"):
            values.append(conn.execute("select max(seq) from " + table).fetchone()[0])
        has_sequence_table = (
            conn.execute("select 1 from sqlite_master where type = 'table' and name = 'sqlite_sequence'").fetchone()
            is not None
        )
        sequence = None
        if has_sequence_table:
            row = conn.execute("select seq from sqlite_sequence where name = 'queue_entries'").fetchone()
            sequence = None if row is None else row[0]
    except sqlite3.Error as err:
        raise StoreError("cannot read the sequence numbers of {}: {}".format(abs_path, _one_line(err)))
    for value in values + [sequence]:
        if value is not None and not _is_int(value):
            raise StoreError("{} holds a sequence number that is not an integer: {!r}".format(abs_path, value))
    present = [v for v in values if v is not None]
    if sequence is None:
        if present:
            raise StoreError("{} has queue rows but no sqlite_sequence row for queue_entries".format(abs_path))
        return 0
    return max(present + [sequence])


def _row_line(kind, row, rule):
    pgid = row.get("child_pgid")
    return "{} seq={} state={} host={} pid={} child_pgid={} rule={} argv={}".format(
        kind,
        row.get("seq"),
        row.get("state"),
        row.get("host_id"),
        row.get("pid"),
        "-" if pgid is None else pgid,
        _one_line(rule),
        _one_line(row.get("argv")),
    )


def _read_failure(message):
    print("queue_retire.py: {}; {}".format(_one_line(message), _READ_REMEDY), file=sys.stderr)
    return EXIT_FAILED


def _cmd_live(args):
    if len(args) != 1:
        print("usage: queue_retire.py live <agent.db>", file=sys.stderr)
        return EXIT_FAILED
    try:
        conn = open_store(args[0])
        try:
            rows = read_rows(conn, args[0])
        finally:
            conn.close()
    except StoreError as err:
        return _read_failure(str(err))

    host = host_identity()
    print("host identity: {}".format(host if host is not None else "unknown (cannot be read; every row blocks)"))
    if sys.platform == "darwin" and (LAYOUT_PROBLEM is not None or layout_problem() is not None):
        print(
            "start times: {}; no start time is trusted, so every row on this boot whose pid exists blocks".format(
                LAYOUT_PROBLEM or layout_problem()
            )
        )
    blocking = 0
    for row in rows:
        blocks, rule = judge_row(host, row)
        if blocks:
            blocking += 1
        print(_row_line("blocking" if blocks else "dead", row, rule))
    print("{} blocking, {} dead".format(blocking, len(rows) - blocking))
    return EXIT_BLOCKED if blocking else EXIT_OK


def _cmd_oldmax(args):
    if len(args) != 1:
        print("usage: queue_retire.py oldmax <agent.db>", file=sys.stderr)
        return EXIT_FAILED
    try:
        conn = open_store(args[0])
        try:
            value = old_max(conn, args[0])
        finally:
            conn.close()
    except StoreError as err:
        return _read_failure(str(err))
    print(value)
    return EXIT_OK


def _cmd_probe_verdict(args):
    if len(args) != 1:
        print("usage: queue_retire.py probe-verdict <exit-code> < probe-stdout", file=sys.stderr)
        return EXIT_FAILED
    try:
        exit_code = int(args[0])
    except ValueError:
        print("queue_retire.py: probe-verdict: exit code {!r} is not an integer".format(args[0]), file=sys.stderr)
        return EXIT_FAILED
    raw = sys.stdin.buffer.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = ""
    verdict, detail = classify_probe(exit_code, text)
    print("{}\t{}".format(verdict, _one_line(detail)))
    return EXIT_OK


_COMMANDS = {
    "live": _cmd_live,
    "oldmax": _cmd_oldmax,
    "probe-verdict": _cmd_probe_verdict,
}


def main(argv):
    if len(argv) < 2 or argv[1] not in _COMMANDS:
        print("usage: queue_retire.py {} ...".format("|".join(sorted(_COMMANDS))), file=sys.stderr)
        return EXIT_FAILED
    try:
        return _COMMANDS[argv[1]](argv[2:])
    except Exception as err:  # fail closed on anything unforeseen
        print("queue_retire.py: {}: unexpected failure: {}; {}".format(argv[1], _one_line(err), _READ_REMEDY), file=sys.stderr)
        return EXIT_FAILED


if __name__ == "__main__":
    sys.exit(main(sys.argv))
