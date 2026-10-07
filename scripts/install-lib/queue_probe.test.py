#!/usr/bin/env python3
"""Tests for the installer's database probe without Python.

Run as ``python3 queue_probe.test.py <planar-agent> <planar> <planar-watch>``;
ctest runs it as ``install.queue_probe`` (label ``install_queue_probe``).

install.sh classifies ``planar-agent queue status 1 --json`` in shell
(``_qr_classify_probe`` in queue-retire.sh) and the database's main schema from
``planar-watch``'s read-only handshake (``planar_db_probe`` in db-probe.sh),
with no python3 (decision 1333). This harness is the only Python involved: it
seeds scratch databases with Python's sqlite3 and runs the functions install.sh
calls under a PATH that holds a few base utilities and NO python3, which it
verifies first.

Oracle: the verdicts and details pinned in ``TABLE`` are what the retired
``queue_retire.py probe-verdict`` (``classify_probe``) returned for the same
(exit status, stdout) pairs; they were taken from that classifier's own
table test (``ProbeVerdict.test_table`` at 54078868) and its detail strings.

Every fixture lives under one scratch directory; HOME, TMPDIR, PLANAR_DB and
PLANAR_HOME all point inside it, and nothing resolves outside it.
"""
import os
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "queue-retire.sh")
AGENT = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else None
PLANAR = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else None
WATCH = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else None

BASE_UTILS = ["tr", "awk", "grep", "sed", "head", "cat", "mktemp", "rm", "dirname", "env", "wc", "readlink"]


def tagged(tag):
    return '{"error":{"verb":"queue status","tag":"%s","message":"m"}}' % tag


# (exit status, stdout, verdict, detail): the oracle.
TABLE = [
    (0, '{"seq":1}', "usable", "exit 0 with a status object"),
    (0, '{"seq": 1, "state": "ended"}\n', "usable", "exit 0 with a status object"),
    (1, tagged("not_found"), "usable", "exit 1, tag not_found"),
    (125, tagged("schema_version_behind"), "behind", "exit 125, tag schema_version_behind"),
    (125, tagged("queue_schema_incompatible"), "incompatible", "exit 125, tag queue_schema_incompatible"),
    (125, tagged("queue_schema_foreign"), "foreign", "exit 125, tag queue_schema_foreign"),
    (125, '{\n  "error": {\n    "verb": "queue status",\n    "tag" : "queue_schema_foreign",\n    "message": "x }"\n  }\n}\n',
     "foreign", "exit 125, tag queue_schema_foreign"),
    (125, tagged("store_unreachable"), "failed", "exit 125 with tag store_unreachable"),
    (1, tagged("schema_version_behind"), "failed", "exit 1 with tag schema_version_behind"),
    (125, tagged("not_found"), "failed", "exit 125 with tag not_found"),
    (0, "not json", "failed", "exit 0 with output that is not JSON"),
    (0, tagged("not_found"), "failed", "exit 0 with tag not_found"),
    (7, tagged("schema_version_ahead"), "failed", "exit 7 with tag schema_version_ahead"),
    (125, "", "failed", "exit 125 with output that is not JSON"),
    (0, "", "failed", "exit 0 with output that is not JSON"),
    (125, '{"error":{"verb":"queue status"}}', "failed", "exit 125 with JSON with no error tag"),
    (2, "error: unexpected argument", "failed", "exit 2 with output that is not JSON"),
    # Hardening (reviewer, 7309): the tag comes from the FIRST top-level error
    # object only, at that object's own depth.
    (125, '{"error":{"message":"has \\"tag\\":\\"queue_schema_foreign\\"","tag":"store_unreachable"}}',
     "failed", "exit 125 with tag store_unreachable"),
    (125, '{"error":{"verb":"a","message":"x"},"tag":"queue_schema_foreign"}',
     "failed", "exit 125 with JSON with no error tag"),
    (125, '{"other":{"tag":"queue_schema_foreign"},"error":{"tag":"store_unreachable"}}',
     "failed", "exit 125 with tag store_unreachable"),
    (125, '{"error":{"tag":"queue_schema_foreign"},"extra":{"error":{"tag":"schema_version_behind"}}}',
     "foreign", "exit 125, tag queue_schema_foreign"),
    (125, '{"error":{"nested":{"tag":"queue_schema_foreign"},"tag":"schema_version_behind"}}',
     "behind", "exit 125, tag schema_version_behind"),
    (125, '{"error":{"nested":{"tag":"queue_schema_foreign"}}}', "failed", "exit 125 with JSON with no error tag"),
    (125, '{"error":{"verb":"a"},"extra":{"tag":"queue_schema_foreign"}}', "failed", "exit 125 with JSON with no error tag"),
    (125, '{"error":"queue_schema_foreign"}', "failed", "exit 125 with JSON with no error tag"),
    (125, '{"error":{"tag":"queue_schema_foreign"}}{"error":{"tag":"schema_version_behind"}}',
     "failed", "exit 125 with output that is not JSON"),
    (125, '{"error":{"tag":"queue_schema_foreign"', "failed", "exit 125 with output that is not JSON"),
    (125, '\ufeff{"error":{"tag":"queue_schema_foreign"}}', "failed", "exit 125 with output that is not JSON"),
    # Invalid JSON or concatenated objects at exit 0 are never usable.
    (0, '{"seq":1}\n{"seq":2}', "failed", "exit 0 with output that is not JSON"),
    (0, '{garbage}', "failed", "exit 0 with output that is not JSON"),
    (0, '{"seq":1,}', "failed", "exit 0 with output that is not JSON"),
    (0, '{"seq":1} trailing', "failed", "exit 0 with output that is not JSON"),
    (0, '[1,2]', "failed", "exit 0 with JSON with no error tag"),
    # An "error" key below the top level does not turn a success into a refusal.
    (0, '{"seq":1,"x":{"error":1}}', "usable", "exit 0 with a status object"),
    (0, '{"seq":1,"x":[{"error":{"tag":"not_found"}}]}', "usable", "exit 0 with a status object"),
    (0, '  {"seq":1}  \r\n', "usable", "exit 0 with a status object"),
]


class Arena:
    """A scratch directory and a PATH with base utilities only."""

    def __init__(self):
        self.root = os.path.realpath(tempfile.mkdtemp(prefix="queue-probe-test-"))
        self.home = os.path.join(self.root, "home")
        self.tmp = os.path.join(self.root, "tmp")
        self.bin = os.path.join(self.root, "pathbin")
        for d in (self.home, self.tmp, self.bin):
            os.makedirs(d)
        for name in BASE_UTILS:
            found = shutil.which(name)
            if found is None:
                raise RuntimeError("base utility missing from the host: " + name)
            os.symlink(found, os.path.join(self.bin, name))
        self.case = 0

    def env(self, prefix=None):
        env = {"PATH": self.bin, "HOME": self.home, "TMPDIR": self.tmp, "LC_ALL": "C"}
        if prefix is not None:
            env["PLANAR_HOME"] = prefix
        return env

    def prefix(self):
        self.case += 1
        p = os.path.join(self.root, "prefix%d" % self.case)
        os.makedirs(os.path.join(p, "bin"))
        return p

    def run(self, prefix, script):
        """Source the libraries as install.sh does, resolve the prefix database
        and run SCRIPT; a database function's state is printed after it."""
        return subprocess.run(
            ["/bin/bash", "-c", 'set -eEuo pipefail; d="$1"; source "$d/prefix-guard.sh"; source "$d/queue-retire.sh"; '
             'source "$d/db-probe.sh"; planar_db_resolve "$PLANAR_HOME"; rc=0; ' + script + ' || rc=$?; '
             'printf "db state: %s %s %s\\n" "${INSTALL_DB_STATE-}" "${INSTALL_DB_VERSION:--}" "${INSTALL_DB_TARGET:--}"; '
             'printf "db detail: %s\\n" "${INSTALL_DB_DETAIL-}"; exit "$rc"', "bash", HERE],
            env=self.env(prefix), cwd=self.root, capture_output=True, text=True, timeout=120)

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


def write_exe(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


class NoPython(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.arena = Arena()

    @classmethod
    def tearDownClass(cls):
        cls.arena.close()

    def test_scratch_path_has_no_python(self):
        got = subprocess.run(["/bin/bash", "-c", "command -v python3 python"], env=self.arena.env(),
                             capture_output=True, text=True)
        self.assertNotEqual(got.returncode, 0, got.stdout)
        self.assertEqual(got.stdout, "")


class Classifier(NoPython):
    def test_table(self):
        for code, out, verdict, detail in TABLE:
            with self.subTest(code=code, out=out):
                got = subprocess.run(
                    ["/bin/bash", "-c", 'set -eEuo pipefail; source "$1"; _qr_classify_probe "$2" "$3"; '
                     'printf "%s\\t%s" "$QR_VERDICT" "$QR_DETAIL"', "bash", LIB, str(code), out],
                    env=self.arena.env(), cwd=self.arena.root, capture_output=True, text=True)
                self.assertEqual(got.returncode, 0, got.stderr)
                self.assertEqual(got.stdout, "%s\t%s" % (verdict, detail))


@unittest.skipUnless(AGENT and PLANAR and WATCH, "needs the built planar-agent, planar and planar-watch")
class RealBinaries(NoPython):
    """The database states of the real binaries against scratch databases."""

    def prefix_with_db(self, mutate=None):
        prefix = self.arena.prefix()
        os.symlink(AGENT, os.path.join(prefix, "bin", "planar-agent"))
        os.symlink(PLANAR, os.path.join(prefix, "bin", "planar"))
        os.symlink(WATCH, os.path.join(prefix, "bin", "planar-watch"))
        db = os.path.join(prefix, "planar.db")
        env = dict(self.arena.env(), PLANAR_DB=db, PLANAR_CONFIG_PATH=os.path.join(prefix, "config.toml"))
        made = subprocess.run([PLANAR, "init", "--skip-project", "--allow-no-repo"], env=env, cwd="/",
                              capture_output=True, text=True)
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        if mutate:
            con = sqlite3.connect(db)
            try:
                mutate(con)
                con.commit()
            finally:
                con.close()
        return prefix, db

    @staticmethod
    def head(db):
        con = sqlite3.connect(db)
        try:
            return con.execute("select max(version) from schema_migrations").fetchone()[0]
        finally:
            con.close()

    def call(self, prefix, fn):
        got = self.arena.run(prefix, '%s "$PLANAR_HOME/bin"' % fn)
        # Never touch anything outside the arena.
        self.assertFalse(os.path.exists(os.path.join(self.arena.home, ".planar")))
        return got

    def test_current_is_current(self):
        prefix, _ = self.prefix_with_db()
        got = self.call(prefix, "planar_db_probe")
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertIn("db state: current - -", got.stdout)

    def test_missing_is_missing_and_init_creates_it(self):
        prefix = self.arena.prefix()
        for name, path in (("planar-agent", AGENT), ("planar", PLANAR), ("planar-watch", WATCH)):
            os.symlink(path, os.path.join(prefix, "bin", name))
        got = self.call(prefix, "planar_db_probe")
        self.assertIn("db state: missing", got.stdout)
        self.assertFalse(os.path.exists(os.path.join(prefix, "planar.db")))
        got = self.call(prefix, "planar_db_migrate")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertIn("db state: current", got.stdout)
        self.assertFalse(os.path.exists(os.path.join(prefix, "config.toml")))

    def test_behind_names_both_versions_with_no_python(self):
        # A deleted head row reads behind to the real planar-watch but is not a
        # chain `planar init` can re-apply, so `planar` here is a stub that
        # migrates nothing: the migration must run init once and then refuse
        # because the database is still behind.
        def behind(con):
            con.execute("delete from schema_migrations where version = (select max(version) from schema_migrations)")
        prefix, db = self.prefix_with_db(behind)
        head = self.head(db)
        got = self.call(prefix, "planar_db_probe")
        self.assertIn("db state: behind %d %d" % (head, head + 1), got.stdout)
        planar = os.path.join(prefix, "bin", "planar")
        os.unlink(planar)
        write_exe(planar, '#!/bin/bash\necho "$*" >> "%s/init.calls"\nexit 0\n' % prefix)
        got = self.call(prefix, "planar_db_migrate")
        self.assertNotEqual(got.returncode, 0)
        self.assertIn("after planar init the database is behind", got.stdout)
        with open(os.path.join(prefix, "init.calls")) as f:
            self.assertEqual(f.read().strip(), "init --skip-project --allow-no-repo")

    def test_ahead_is_ahead_even_with_a_compatible_queue(self):
        def ahead(con):
            con.execute("insert into schema_migrations (version, description) values ((select max(version) from schema_migrations) + 1, 'newer')")
        prefix, db = self.prefix_with_db(ahead)
        head = self.head(db)
        got = self.call(prefix, "planar_db_probe")
        self.assertIn("db state: ahead %d %d" % (head, head - 1), got.stdout)

    def test_foreign_and_incompatible_queues_are_faults(self):
        prefix, _ = self.prefix_with_db(lambda con: con.execute("drop table queue_schema"))
        got = self.call(prefix, "planar_db_probe")
        self.assertIn("db state: fault", got.stdout)
        self.assertIn("queue_schema_foreign", got.stdout)

        def incompatible(con):
            con.execute("insert into queue_schema (version, compat, description) values (2, 2, 'needs a newer binary')")
        prefix, _ = self.prefix_with_db(incompatible)
        got = self.call(prefix, "planar_db_probe")
        self.assertIn("db state: fault", got.stdout)

    def test_corrupt_is_a_fault_with_its_diagnostic(self):
        prefix, db = self.prefix_with_db()
        with open(db, "wb") as f:
            f.write(b"not a database at all " * 300)
        got = self.call(prefix, "planar_db_probe")
        self.assertIn("db state: fault", got.stdout)
        self.assertIn("not a database", got.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)
