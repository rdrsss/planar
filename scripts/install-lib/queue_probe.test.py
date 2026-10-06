#!/usr/bin/env python3
"""Tests for the queue-store probe in scripts/install-lib/queue-retire.sh.

Run as ``python3 queue_probe.test.py <planar-agent> <planar>``; ctest runs it
as ``install.queue_probe`` (label ``install_queue_probe``).

install.sh classifies ``planar-agent queue status 1 --json`` in shell, with no
python3 (decision 1333). This harness is the only Python involved: it seeds
scratch databases with Python's sqlite3 and runs ``queue_probe_migrate`` --
the function install.sh calls -- under a PATH that holds a few base utilities
and NO python3, which it verifies first.

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

BASE_UTILS = ["tr", "awk", "grep", "sed", "head", "cat", "mktemp", "rm", "dirname", "env"]


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
        return subprocess.run(
            ["/bin/bash", "-c", 'set -eEuo pipefail; source "$1"; ' + script, "bash", LIB],
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


class StubProbe(NoPython):
    """queue_probe_migrate against a stub planar-agent, with no python3."""

    def stub(self, code, out, err=""):
        prefix = self.arena.prefix()
        with open(os.path.join(prefix, "stub.out"), "w") as f:
            f.write(out)
        with open(os.path.join(prefix, "stub.err"), "w") as f:
            f.write(err)
        write_exe(os.path.join(prefix, "bin", "planar-agent"),
                  '#!/bin/bash\ncat "%s/stub.out"\ncat "%s/stub.err" >&2\nexit %d\n' % (prefix, prefix, code))
        write_exe(os.path.join(prefix, "bin", "planar"), "#!/bin/bash\nexit 0\n")
        with open(os.path.join(prefix, "planar.db"), "w") as f:
            f.write("db\n")
        return prefix

    def test_each_table_row_through_queue_probe_migrate(self):
        # behind needs a migration (not a stub concern); the rest end the step.
        for code, out, verdict, detail in TABLE:
            if verdict == "behind":
                continue
            with self.subTest(code=code, out=out):
                prefix = self.stub(code, out)
                got = self.arena.run(prefix, "queue_probe_migrate")
                self.assertIn("queue store probe: %s (%s)" % (verdict, detail), got.stdout)
                if verdict == "failed":
                    self.assertNotEqual(got.returncode, 0)
                    self.assertIn("agent.db was NOT retired", got.stderr)
                else:
                    self.assertEqual(got.returncode, 0, got.stderr)

    def test_failed_probe_reports_the_probes_stderr(self):
        prefix = self.stub(125, "garbage", err="store exploded here")
        got = self.arena.run(prefix, "queue_probe_migrate")
        self.assertNotEqual(got.returncode, 0)
        self.assertIn("store exploded here", got.stderr)

    def test_behind_migrates_then_must_be_usable(self):
        prefix = self.stub(125, tagged("schema_version_behind"))
        # The stub answers behind every time, so the post-migration probe fails.
        got = self.arena.run(prefix, "queue_probe_migrate")
        self.assertNotEqual(got.returncode, 0)
        self.assertIn("still not usable after the migration", got.stderr)


@unittest.skipUnless(AGENT and PLANAR, "needs the built planar-agent and planar")
class RealBinaries(NoPython):
    """The three verdicts of the real binaries against scratch databases."""

    def prefix_with_db(self, mutate=None):
        prefix = self.arena.prefix()
        os.symlink(AGENT, os.path.join(prefix, "bin", "planar-agent"))
        os.symlink(PLANAR, os.path.join(prefix, "bin", "planar"))
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
    def head(con):
        return con.execute("select max(version) from schema_migrations").fetchone()[0]

    def probe(self, prefix):
        got = self.arena.run(prefix, "queue_probe_migrate")
        # Never touch anything outside the arena.
        self.assertFalse(os.path.exists(os.path.join(self.arena.home, ".planar")))
        return got

    def test_current_is_usable(self):
        prefix, _ = self.prefix_with_db()
        got = self.probe(prefix)
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertRegex(got.stdout, r"queue store probe: usable \(exit [01], ")
        self.assertNotIn("migrating", got.stdout)

    def test_behind_is_classified_behind_with_no_python(self):
        # A deleted head row reads behind to the real planar-agent but is not
        # a chain `planar init` can re-apply, so `planar` here is a stub that
        # migrates nothing: the step must name the behind verdict, run init
        # once, and refuse because the database is still behind.
        def behind(con):
            con.execute("delete from schema_migrations where version = (select max(version) from schema_migrations)")
        prefix, _ = self.prefix_with_db(behind)
        planar = os.path.join(prefix, "bin", "planar")
        os.unlink(planar)
        write_exe(planar, '#!/bin/bash\necho "$*" >> "%s/init.calls"\nexit 0\n' % prefix)
        got = self.probe(prefix)
        self.assertIn("queue store probe: behind (exit 125, tag schema_version_behind)", got.stdout)
        self.assertIn("migrating it", got.stdout)
        self.assertNotEqual(got.returncode, 0)
        self.assertIn("still not usable after the migration (behind:", got.stderr)
        with open(os.path.join(prefix, "init.calls")) as f:
            self.assertEqual(f.read().strip(), "init --skip-project --allow-no-repo")

    def test_foreign_is_warned_not_refused(self):
        prefix, _ = self.prefix_with_db(lambda con: con.execute("drop table queue_schema"))
        got = self.probe(prefix)
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertIn("queue store probe: foreign (exit 125, tag queue_schema_foreign)", got.stdout)
        self.assertIn("same number, foreign migration", got.stderr)

    def test_incompatible_is_warned_not_refused(self):
        def incompatible(con):
            con.execute("insert into queue_schema (version, compat, description) values (2, 2, 'needs a newer binary')")
            con.execute("insert into schema_migrations (version, description) values ((select max(version) from schema_migrations) + 1, 'newer')")
        prefix, _ = self.prefix_with_db(incompatible)
        got = self.probe(prefix)
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertIn("queue store probe: incompatible (exit 125, tag queue_schema_incompatible)", got.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)
