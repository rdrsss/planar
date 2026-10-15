#!/usr/bin/env python3
"""Unit tests for `acceptance.py`, the three-orchestrator acceptance eval.

Provider-free and build-free: no model is spawned and no Planar binary is run.
The fixture builder is driven through a recording runner, the bundle through
stub executables, and the grader through a shell-script `planar` that prints a
canned report, so these pin the harness around the checker: the fixture's
shape, the sandbox environment, the three command lines and the grading rules.
"""
from __future__ import annotations

import json
import os
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import acceptance
import trail


def report(violations: list[dict] | None = None, claims: int = 4) -> dict:
    found = violations or []
    return {
        "plan_id": 1,
        "plans": 3,
        "conforms": not found,
        "floor": {"claim_id": 1, "origin": "explicit", "cut_at": ""},
        "graded": {"claims": claims, "done_tasks": 4},
        "violations": found,
        "overrides": [],
        "policy_changed": [],
        "before_floor": {"claims": 0, "done_tasks": 0},
    }


VIOLATION = {
    "rule": 6,
    "name": "independent-review",
    "entity_kind": "claim",
    "entity_id": 3,
    "reason": "completed without a review from another session",
}
ALL_DONE = {spec.slug: "done" for spec in acceptance.FIXTURE_TASKS}


class Recorder:
    """A runner that records each argv and answers like the CLIs do."""

    def __init__(self, packets: dict[int, dict] | None = None):
        self.calls: list[list[str]] = []
        self.packets = packets or {}
        self.next_plan = 1
        self.next_task = 1

    def __call__(self, argv):
        argv = list(argv)
        self.calls.append(argv)
        if argv[:2] == ["planar", "init"]:
            return json.dumps({"ok": True, "project_id": 1, "project_slug": "calc"})
        if argv[:3] == ["planar", "plan", "create"]:
            self.next_plan += 1
            return json.dumps({"id": self.next_plan - 1})
        if argv[:3] == ["planar", "task", "add"]:
            self.next_task += 1
            return json.dumps({"id": self.next_task - 1})
        if argv[:3] == ["planar", "task", "packet"]:
            task_id = int(argv[3])
            return json.dumps(self.packets.get(task_id, {"ready": True, "reasons": []}))
        return ""


def fixture_in(tmp: Path) -> acceptance.Fixture:
    env = acceptance.sandbox_env(tmp, base={"HOME": "/nonexistent-operator"}, operator_home=Path("/nonexistent-operator"))
    Path(env["PLANAR_HOME"]).mkdir(parents=True)
    return acceptance.Fixture(root=tmp, env=env, repo=tmp / "repo")


class FixtureShapeTest(unittest.TestCase):
    def test_first_milestone_is_a_wave_of_three_disjoint_tasks(self):
        wave = [spec for spec in acceptance.FIXTURE_TASKS if spec.milestone == 0]
        self.assertEqual(len(wave), 3)
        self.assertTrue(all(not spec.depends for spec in wave))
        touched = [path for spec in acceptance.FIXTURE_TASKS for path in (spec.path, spec.test_path)]
        self.assertEqual(len(touched), len(set(touched)), "touches must be disjoint")

    def test_second_milestone_waits_on_the_whole_wave(self):
        gate = [spec for spec in acceptance.FIXTURE_TASKS if spec.milestone == 1]
        self.assertEqual(len(gate), 1)
        wave = {spec.slug for spec in acceptance.FIXTURE_TASKS if spec.milestone == 0}
        self.assertEqual(set(gate[0].depends), wave)

    def test_policy_requires_review_queue_and_gate(self):
        text = acceptance.policy_toml()
        self.assertIn('[associations."project:calc".policy]', text)
        self.assertIn("review_required = true", text)
        self.assertIn("use_queue = true", text)
        self.assertIn('task_gate = ["sh", "check.sh"]', text)

    def test_repo_is_committed_and_its_gate_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "repo"
            acceptance.write_repo(repo)
            log = subprocess.run(["git", "-C", str(repo), "log", "--format=%B"], capture_output=True, text=True, check=True)
            self.assertIn("Signed-off-by:", log.stdout)
            status = subprocess.run(["git", "-C", str(repo), "status", "--porcelain"], capture_output=True, text=True, check=True)
            self.assertEqual(status.stdout, "")
            self.assertTrue(os.access(repo / "check.sh", os.X_OK))
            gate = subprocess.run(["sh", "check.sh"], cwd=repo, capture_output=True, text=True, check=False)
            self.assertEqual(gate.returncode, 0, gate.stderr)


class FixturePlanTest(unittest.TestCase):
    def test_plan_is_made_through_the_cli_with_policy_touches_and_dependencies(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = fixture_in(Path(tmp))
            runner = Recorder()
            acceptance.create_fixture_plan(fixture, runner)
            self.assertTrue(all(call[0] == "planar" for call in runner.calls))
            self.assertIn(["planar", "assoc", "create", "project:calc", "--kind", "project"], runner.calls)
            self.assertIn(["planar", "assoc", "add", "project:calc", str(fixture.repo)], runner.calls)
            self.assertIn("use_queue = true", fixture.config.read_text())
            validate = runner.calls.index(["planar", "config", "validate"])
            first_plan = next(i for i, call in enumerate(runner.calls) if call[:3] == ["planar", "plan", "create"])
            self.assertLess(validate, first_plan, "the policy is validated before any plan exists")
            self.assertEqual(fixture.anchor_id, 1)
            self.assertEqual(fixture.milestone_ids, [2, 3])
            milestone_args = [call for call in runner.calls if "--parent" in call]
            self.assertEqual([call[call.index("--parent") + 1] for call in milestone_args], ["1", "1"])
            for spec in acceptance.FIXTURE_TASKS:
                task_id = str(fixture.task_ids[spec.slug])
                for path in (spec.path, spec.test_path):
                    self.assertIn(["planar", "task", "touches", "add", task_id, "calc", "--path", path], runner.calls)
            dispatch = str(fixture.task_ids["acc-calc-dispatch"])
            links = [call for call in runner.calls if call[:3] == ["planar", "task", "link"]]
            self.assertEqual(len(links), 3)
            for call in links:
                self.assertEqual(call[3], dispatch)
                self.assertEqual(call[-2:], ["--relationship", "depends-on"])

    def test_readiness_accepts_a_ready_wave_and_a_gated_dispatcher(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = fixture_in(Path(tmp))
            acceptance.create_fixture_plan(fixture, Recorder())
            gated = {fixture.task_ids["acc-calc-dispatch"]: {"ready": False, "reasons": ["blocked_by_open_task"]}}
            reasons = acceptance.check_readiness(fixture, Recorder(gated))
            self.assertEqual(reasons["acc-calc-dispatch"], ["blocked_by_open_task"])

    def test_readiness_refuses_an_unready_wave_task(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = fixture_in(Path(tmp))
            acceptance.create_fixture_plan(fixture, Recorder())
            packets = {
                fixture.task_ids["acc-calc-add"]: {"ready": False, "reasons": ["open_question_linked"]},
                fixture.task_ids["acc-calc-dispatch"]: {"ready": False, "reasons": ["blocked_by_open_task"]},
            }
            with self.assertRaisesRegex(acceptance.AcceptanceError, "acc-calc-add is not ready"):
                acceptance.check_readiness(fixture, Recorder(packets))

    def test_readiness_refuses_a_dispatcher_held_by_anything_else(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = fixture_in(Path(tmp))
            acceptance.create_fixture_plan(fixture, Recorder())
            ungated = {fixture.task_ids["acc-calc-dispatch"]: {"ready": True, "reasons": []}}
            with self.assertRaisesRegex(acceptance.AcceptanceError, "acc-calc-dispatch should wait"):
                acceptance.check_readiness(fixture, Recorder(ungated))

    def test_runner_refuses_a_binary_outside_the_sandbox(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = fixture_in(Path(tmp))
            runner = acceptance.make_runner(fixture.env, Path(tmp), Path(tmp) / "log")
            with self.assertRaisesRegex(acceptance.AcceptanceError, "not an installed Planar binary"):
                runner(["sh", "-c", "true"])
            with self.assertRaisesRegex(acceptance.AcceptanceError, "not an installed Planar binary"):
                runner(["planar", "version"])  # not installed in this sandbox


class EnvironmentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name).resolve()
        self.operator = base / "operator"
        (self.operator / ".local" / "bin").mkdir(parents=True)
        (self.operator / ".planar").mkdir()
        self.root = base / "sandbox"
        self.root.mkdir()
        self.base = {
            "HOME": str(self.operator),
            "PATH": f"{self.operator / '.local' / 'bin'}:/usr/bin:/bin",
            "PLANAR_DB": str(self.operator / ".planar" / "planar.db"),
            "PLANAR_HOME": str(self.operator / ".planar"),
            "PLANAR_CONFIG_PATH": str(self.operator / ".planar" / "config.toml"),
            "PLANAR_SCOPE": "repo:planar",
            "CLAUDECODE": "1",
            "CLAUDE_CODE_ENTRYPOINT": "cli",
            "ANTHROPIC_API_KEY": "sk-ant-test",
            "OPENAI_API_KEY": "sk-openai-test",
            "LANG": "en_US.UTF-8",
            "SOMETHING_ELSE": str(self.operator / "x"),
        }

    def tearDown(self):
        self.tmp.cleanup()

    def env(self, vendor):
        return acceptance.sandbox_env(self.root, base=self.base, vendor=vendor, operator_home=self.operator)

    def test_every_path_is_in_the_sandbox_and_none_in_the_operator_home(self):
        for vendor in (None, "claude", "codex"):
            env = self.env(vendor)
            acceptance.assert_sandboxed(env, self.root, self.operator)
            for name in ("HOME", "PLANAR_HOME", "PLANAR_DB"):
                self.assertTrue(Path(env[name]).is_relative_to(self.root), name)
            self.assertEqual(Path(env["PLANAR_DB"]).parent, Path(env["PLANAR_HOME"]))
            for name, value in env.items():
                if name.endswith("_API_KEY"):
                    continue
                self.assertNotIn(str(self.operator), value, name)
            self.assertEqual(env["PATH"].split(os.pathsep)[0], str(self.root / "home" / ".planar" / "bin"))

    def test_nothing_but_the_vendors_own_credential_crosses(self):
        claude = self.env("claude")
        codex = self.env("codex")
        neutral = self.env(None)
        self.assertEqual(claude.get("ANTHROPIC_API_KEY"), "sk-ant-test")
        self.assertNotIn("OPENAI_API_KEY", claude)
        self.assertEqual(codex.get("OPENAI_API_KEY"), "sk-openai-test")
        self.assertNotIn("ANTHROPIC_API_KEY", codex)
        for env in (claude, codex, neutral):
            for leaked in ("PLANAR_SCOPE", "CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT", "SOMETHING_ELSE"):
                self.assertNotIn(leaked, env)
            self.assertEqual(env["LANG"], "en_US.UTF-8")
        self.assertNotIn("ANTHROPIC_API_KEY", neutral)

    def test_the_real_environment_cannot_leak_into_a_child(self):
        home = acceptance.real_home()
        for vendor in (None, "claude", "codex"):
            env = acceptance.sandbox_env(self.root, vendor=vendor)
            acceptance.assert_sandboxed(env, self.root)
            for name, value in env.items():
                if name in acceptance.AUTH_ENV_VARS["claude"] + acceptance.AUTH_ENV_VARS["codex"]:
                    continue
                for piece in value.split(os.pathsep):
                    if piece.startswith("/"):
                        self.assertFalse(Path(piece).resolve().is_relative_to(home), f"{name}={piece}")
                        self.assertFalse(Path(piece).resolve().is_relative_to(home / ".planar"), f"{name}={piece}")

    def test_path_drops_system_directories_under_the_operator_home(self):
        entries = acceptance.system_path_entries(Path("/usr"))
        self.assertNotIn("/usr/bin", entries)
        self.assertIn("/bin", entries)

    def test_assert_sandboxed_refuses_each_escape(self):
        good = self.env("claude")
        escapes = {
            "PLANAR_DB": str(self.operator / ".planar" / "planar.db"),
            "PLANAR_HOME": str(self.operator / ".planar"),
            "HOME": str(self.operator),
            "PLANAR_CONFIG_PATH": "relative/config.toml",
            "TMPDIR": "/tmp",
        }
        for name, value in escapes.items():
            with self.subTest(name=name):
                with self.assertRaises(acceptance.AcceptanceError):
                    acceptance.assert_sandboxed({**good, name: value}, self.root, self.operator)
        with self.assertRaisesRegex(acceptance.AcceptanceError, "PATH entry"):
            acceptance.assert_sandboxed({**good, "PATH": f"{self.operator / '.local' / 'bin'}:/usr/bin"}, self.root, self.operator)
        with self.assertRaisesRegex(acceptance.AcceptanceError, "reaches into the operator's home"):
            acceptance.assert_sandboxed({**good, "EXTRA": str(self.operator / ".planar" / "x")}, self.root, self.operator)
        inside = self.operator / "sandbox"
        inside.mkdir()
        with self.assertRaisesRegex(acceptance.AcceptanceError, "lies under the operator's home"):
            acceptance.assert_sandboxed(
                acceptance.sandbox_env(inside, base=self.base, operator_home=self.operator), inside, self.operator
            )
        with self.assertRaises(acceptance.AcceptanceError):
            missing = dict(good)
            del missing["PLANAR_DB"]
            acceptance.assert_sandboxed(missing, self.root, self.operator)

    def test_a_sandbox_under_an_instruction_file_is_refused(self):
        (self.root / "CLAUDE.md").write_text("doctrine\n")
        nested = self.root / "a" / "b"
        nested.mkdir(parents=True)
        with self.assertRaisesRegex(acceptance.AcceptanceError, "CLAUDE.md"):
            acceptance.assert_no_ancestor_instructions(nested)

    def test_vendor_shims_keep_path_out_of_the_operator_home(self):
        target = self.operator / ".local" / "bin" / "claude"
        target.write_text("#!/bin/sh\n")
        acceptance.make_vendor_shims(self.root, {"claude": target})
        link = self.root / "vendor-bin" / "claude"
        self.assertTrue(link.is_symlink())
        self.assertEqual(Path(os.readlink(link)), target)
        self.assertIn(str(self.root / "vendor-bin"), self.env("claude")["PATH"].split(os.pathsep))


class BundleTest(unittest.TestCase):
    def test_bundle_has_the_release_layout_and_no_methodology(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            for name in acceptance.PLANAR_BINARIES:
                (bin_dir / name).write_text("#!/bin/sh\nexit 0\n")
                (bin_dir / name).chmod(0o755)
            bundle = acceptance.assemble_bundle(Path(tmp) / "bundle", bin_dir=bin_dir)
            for relative in (
                "install.sh", "uninstall.sh", "get-planar.sh", "install-cleanup.txt", "release.json",
                "skills/planar/SKILL.md", "scripts/install-lib/data-paths.sh",
            ):
                self.assertTrue((bundle / relative).is_file(), relative)
            for name in acceptance.PLANAR_BINARIES:
                self.assertTrue(os.access(bundle / "bin" / name, os.X_OK), name)
            self.assertFalse((bundle / "agents").exists())
            self.assertFalse((bundle / "codex-agents").exists())
            self.assertFalse(list(bundle.rglob("__pycache__")))
            release = json.loads((bundle / "release.json").read_text())
            self.assertEqual(release["schema_version"], acceptance.migration_head())
            self.assertRegex(release["sha"], r"^[0-9a-f]{40}$")

    def test_bundle_refuses_a_missing_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(acceptance.AcceptanceError, "lacks planar"):
                acceptance.assemble_bundle(Path(tmp) / "bundle", bin_dir=Path(tmp))

    def test_check_install_refuses_placed_methodology(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = fixture_in(Path(tmp))
            env = fixture.env
            bin_dir = Path(env["PLANAR_HOME"]) / "bin"
            bin_dir.mkdir()
            for name in acceptance.PLANAR_BINARIES:
                (bin_dir / name).write_text("#!/bin/sh\n")
                (bin_dir / name).chmod(0o755)
            for skills in (".claude/skills/planar", ".agents/skills/planar"):
                (Path(env["HOME"]) / skills).mkdir(parents=True)
                (Path(env["HOME"]) / skills / "SKILL.md").write_text("skill\n")
            Path(env["PLANAR_DB"]).write_text("")
            acceptance.check_install(env)
            (Path(env["HOME"]) / ".claude" / "agents").mkdir()
            with self.assertRaisesRegex(acceptance.AcceptanceError, "placed methodology"):
                acceptance.check_install(env)


class CommandTest(unittest.TestCase):
    def fixture(self, tmp: Path) -> acceptance.Fixture:
        fixture = fixture_in(tmp)
        fixture.anchor_id = 17
        return fixture

    def test_stock_runs_are_told_only_to_use_planar(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.fixture(Path(tmp))
            for name in ("claude-stock", "codex-stock"):
                run = acceptance.RUNS[name]
                prompt = acceptance.prompt_for(run, fixture)
                self.assertEqual(prompt, "use planar for plan 17")
                self.assertEqual(acceptance.build_command(run, fixture, prompt)[-1], prompt)

    def test_claude_runs_unattended_with_the_sandbox_writable_and_no_agent(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.fixture(Path(tmp))
            for name in ("claude-stock", "claude-swarm"):
                run = acceptance.RUNS[name]
                argv = acceptance.build_command(run, fixture, "P", model="opus")
                self.assertEqual(argv[:2], ["claude", "-p"])
                self.assertIn("--dangerously-skip-permissions", argv)
                self.assertEqual(argv[argv.index("--add-dir") + 1], str(fixture.root))
                self.assertEqual(argv[argv.index("--model") + 1], "opus")
                self.assertEqual(argv[argv.index("--output-format") + 1], "stream-json")
                self.assertNotIn("--agent", argv)
                self.assertNotIn("--allowedTools", " ".join(argv))

    def test_codex_runs_in_its_workspace_sandbox_rooted_at_the_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.fixture(Path(tmp))
            argv = acceptance.build_command(acceptance.RUNS["codex-stock"], fixture, "P")
            self.assertEqual(argv[:3], ["codex", "exec", "--json"])
            self.assertEqual(argv[argv.index("--sandbox") + 1], "workspace-write")
            self.assertEqual(argv[argv.index("--add-dir") + 1], str(fixture.root))
            self.assertEqual(argv[argv.index("-C") + 1], str(fixture.repo))
            self.assertNotIn("--dangerously-bypass-approvals-and-sandbox", argv)
            self.assertNotIn("--model", argv)

    def test_swarm_prompt_fills_every_placeholder_from_the_example(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.fixture(Path(tmp))
            prompt = acceptance.prompt_for(acceptance.RUNS["claude-swarm"], fixture)
            self.assertTrue(prompt.startswith("You are the orchestrator for Planar plan 17."))
            self.assertIn(f"Checkout: {fixture.repo}.", prompt)
            self.assertIn("Worker model: haiku.", prompt)
            self.assertIn("Wave size: 3.", prompt)
            for placeholder in ("<plan-id>", "<repo-checkout>", "<plan-branch>", "<plan-worktree>", "<worker-model>", "<writer-model>", "<n>"):
                self.assertNotIn(placeholder, prompt)
            self.assertIn("orchestrator merges into the plan branch; the\noperator merges the plan branch.", prompt)
            self.assertIn("planar health conformance 17", prompt)

    def test_swarm_template_is_the_examples_fenced_prompt(self):
        template = acceptance.swarm_template()
        self.assertIn("<plan-id>", template)
        self.assertIn(template, acceptance.SWARM_EXAMPLE.read_text())

    def test_unknown_run_is_a_usage_error(self):
        with self.assertRaises(SystemExit):
            acceptance.parse_args(["--runs", "claude-stock,gemini"])


class AuthTest(unittest.TestCase):
    def test_no_credential_blocks_without_spawning(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = acceptance.sandbox_env(Path(tmp), base={"HOME": "/nonexistent"}, vendor="codex", operator_home=Path("/nonexistent"))
            with mock.patch.object(acceptance, "run_host", side_effect=AssertionError("spawned")):
                problem = acceptance.auth_problem("codex", env, Path(tmp), Path(tmp) / "a")
                self.assertIn("OPENAI_API_KEY", problem)
                self.assertIn("never copied", problem)
                problem = acceptance.auth_problem("claude", env, Path(tmp), Path(tmp) / "a")
                self.assertIn("CLAUDE_CODE_OAUTH_TOKEN", problem)

    def test_probe_commands_are_one_shot(self):
        self.assertEqual(acceptance.probe_command("claude"), ["claude", "-p", "say ok"])
        self.assertEqual(acceptance.probe_command("codex")[:2], ["codex", "exec"])

    def test_host_timeout_kills_the_process_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            out, err = Path(tmp) / "o", Path(tmp) / "e"
            result = acceptance.run_host(["sh", "-c", "sleep 30 & wait"], {"PATH": "/usr/bin:/bin"}, Path(tmp), 1, out, err)
            self.assertTrue(result.timed_out)
            self.assertIsNone(result.exit_code)
            self.assertLess(result.seconds, 20)


class GradeTest(unittest.TestCase):
    def grade(self, violations=None, claims=4, tasks=None):
        parsed = trail.parse_report(json.dumps(report(violations, claims)), 1 if violations else 0)
        return acceptance.judge(parsed, None, tasks or ALL_DONE)

    def test_a_conforming_finished_trail_passes(self):
        verdict = self.grade()
        self.assertEqual(verdict.grade, "pass")
        self.assertEqual(verdict.reasons, [])

    def test_any_violation_fails_and_names_its_rule(self):
        verdict = self.grade([VIOLATION])
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("rule 6 independent-review", verdict.reasons[0])
        self.assertEqual(acceptance.violations_by_rule(verdict.report), {"6:independent-review": 1})

    def test_an_empty_trail_fails(self):
        verdict = self.grade(claims=0)
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("graded no claims", " ".join(verdict.reasons))

    def test_an_unfinished_plan_fails(self):
        verdict = self.grade(tasks={**ALL_DONE, "acc-calc-dispatch": "todo"})
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("acc-calc-dispatch=todo", " ".join(verdict.reasons))

    def test_an_unreadable_report_fails(self):
        verdict = acceptance.judge(None, "conformance report is not JSON", ALL_DONE)
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("unreadable", verdict.reasons[0])


class GradeFixtureTest(unittest.TestCase):
    """`grade_fixture` end to end over a stub `planar` and a real SQLite file."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fixture = fixture_in(Path(self.tmp.name).resolve())
        self.fixture.anchor_id = 1
        self.fixture.task_ids = {spec.slug: index + 1 for index, spec in enumerate(acceptance.FIXTURE_TASKS)}
        self.fixture.config.write_text(acceptance.policy_toml())
        connection = sqlite3.connect(self.fixture.database)
        connection.execute("create table tasks (id integer primary key, status text)")
        for task_id in self.fixture.task_ids.values():
            connection.execute("insert into tasks values (?, 'done')", (task_id,))
        connection.commit()
        connection.close()
        self.planar = Path(self.tmp.name) / "planar"

    def tearDown(self):
        self.tmp.cleanup()

    def stub(self, stdout: str, code: int) -> None:
        payload = Path(self.tmp.name) / "payload"
        payload.write_text(stdout)
        self.planar.write_text(f"#!/bin/sh\ncat '{payload}'\nexit {code}\n")
        self.planar.chmod(self.planar.stat().st_mode | stat.S_IXUSR)

    def test_conforming_report_passes(self):
        self.stub(json.dumps(report()), 0)
        self.assertEqual(acceptance.grade_fixture(self.fixture, self.planar).grade, "pass")

    def test_violating_report_fails(self):
        self.stub(json.dumps(report([VIOLATION])), 1)
        verdict = acceptance.grade_fixture(self.fixture, self.planar)
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("rule 6", verdict.reasons[0])

    def test_garbage_report_fails_as_unreadable(self):
        self.stub("not json at all", 0)
        verdict = acceptance.grade_fixture(self.fixture, self.planar)
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("unreadable", verdict.reasons[0])

    def test_refusal_is_not_a_verdict(self):
        self.stub("", 2)
        verdict = acceptance.grade_fixture(self.fixture, self.planar)
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("unreadable", verdict.reasons[0])

    def test_report_that_disagrees_with_its_exit_code_fails(self):
        self.stub(json.dumps(report()), 1)
        self.assertEqual(acceptance.grade_fixture(self.fixture, self.planar).grade, "fail")

    def assert_unreadable(self, verdict: acceptance.RunGrade) -> None:
        self.assertEqual(verdict.grade, "fail")
        self.assertIn("unreadable", verdict.reasons[0])
        self.assertEqual(verdict.tasks, {})

    def test_missing_database_is_an_unreadable_report_not_an_exception(self):
        self.stub(json.dumps(report()), 0)
        self.fixture.database.unlink()
        self.assert_unreadable(acceptance.grade_fixture(self.fixture, self.planar))

    def test_corrupt_database_is_an_unreadable_report_not_an_exception(self):
        self.stub(json.dumps(report()), 0)
        self.fixture.database.write_bytes(b"this is not a database " * 200)
        with mock.patch.object(acceptance.trail, "grade_database", side_effect=AssertionError("must not be reached")):
            self.assert_unreadable(acceptance.grade_fixture(self.fixture, self.planar))

    def test_sqlite_error_while_grading_is_an_unreadable_report(self):
        with mock.patch.object(acceptance.trail, "grade_database", side_effect=sqlite3.OperationalError("database is locked")):
            verdict = acceptance.grade_fixture(self.fixture, self.planar)
        self.assert_unreadable(verdict)
        self.assertIn("database is locked", verdict.reasons[0])

    def test_unreadable_policy_file_is_an_unreadable_report(self):
        self.stub(json.dumps(report()), 0)
        self.fixture.config.unlink()
        self.assert_unreadable(acceptance.grade_fixture(self.fixture, self.planar))


class ExecuteRunTest(unittest.TestCase):
    """`execute_run` records a database or file error as that run's failure and returns."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.results = self.root / "results"
        self.sandboxes = self.root / "sandboxes"
        self.sandboxes.mkdir()
        self.run = acceptance.RUNS["claude-swarm"]

    def tearDown(self):
        self.tmp.cleanup()

    def execute(self) -> dict:
        return acceptance.execute_run(
            self.run, self.results, self.sandboxes, bin_dir=self.root / "bin",
            timeout_minutes=1, model=None, keep=False, ledger=False,
        )

    def test_a_database_error_building_the_sandbox_fails_the_run_and_returns(self):
        with mock.patch.object(acceptance, "vendor_binaries", return_value={"claude": Path("/bin/true")}), \
                mock.patch.object(acceptance, "build_sandbox", side_effect=sqlite3.OperationalError("disk I/O error")):
            summary = self.execute()
        self.assertEqual(summary["status"], "fail")
        self.assertIn("disk I/O error", summary["reasons"][0])
        written = json.loads((self.results / self.run.name / "grade.json").read_text())
        self.assertEqual(written["status"], "fail")

    def test_a_failed_snapshot_keeps_the_grade_and_names_the_snapshot(self):
        fixture = fixture_in(self.root / "fixture")
        passed = acceptance.RunGrade("pass", [], report(), dict(ALL_DONE))
        with mock.patch.object(acceptance, "vendor_binaries", return_value={"claude": Path("/bin/true")}), \
                mock.patch.object(acceptance, "build_sandbox", return_value=fixture), \
                mock.patch.object(acceptance, "auth_problem", return_value=None), \
                mock.patch.object(acceptance, "prompt_for", return_value="prompt"), \
                mock.patch.object(acceptance, "build_command", return_value=["claude", "prompt"]), \
                mock.patch.object(acceptance, "run_host", return_value=acceptance.HostResult(0, 1.0, False)), \
                mock.patch.object(acceptance, "grade_fixture", return_value=passed), \
                mock.patch.object(acceptance.trail, "snapshot_database", side_effect=OSError("no space left")):
            summary = self.execute()
        self.assertEqual(summary["status"], "pass")
        self.assertIn("the database snapshot was not saved: no space left", summary["reasons"])


class LedgerTest(unittest.TestCase):
    def test_row_follows_the_ledger_format(self):
        import harness

        with tempfile.TemporaryDirectory() as tmp:
            ledger = Path(tmp) / "RESULTS.md"
            artifact = Path(tmp) / "claude-swarm"
            artifact.mkdir()
            (artifact / "grade.json").write_text('{"status": "pass"}\n')
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger):
                acceptance.record_ledger(acceptance.RUNS["claude-swarm"], artifact, "pass", None)
                rows = harness.ledger_rows()
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertEqual(row["case"], "three-orchestrators/claude-swarm")
            self.assertEqual(
                (row["mode"], row["vendor"], row["surface"], row["model"], row["grade"]),
                ("acceptance", "claude", "swarm", "-", "pass"),
            )
            self.assertEqual(row["artifact hash"], acceptance.artifact_hash(artifact))


if __name__ == "__main__":
    unittest.main()
