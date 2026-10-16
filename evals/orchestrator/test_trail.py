#!/usr/bin/env python3
"""Unit tests for `trail.py` and the harness code that grades a trail.

Provider-free and binary-free: a `planar` is replaced by a shell script that
records how it was called and prints a canned report, so these tests pin what
the grader does around the checker (scratch isolation, the report contract, the
expectation logic) without needing a build. The checker itself, and the
recorded trails that exercise it, run in the `--trails` lane.
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import sqlite3
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness
import test_harness
import trail


def sample_report(violations: list[dict] | None = None, overrides: list[dict] | None = None) -> dict:
    found = violations or []
    return {
        "plan_id": 1,
        "plans": 1,
        "conforms": not found,
        "floor": {"claim_id": 1, "origin": "explicit", "cut_at": ""},
        "graded": {"claims": 1, "done_tasks": 1},
        "violations": found,
        "overrides": overrides or [],
        "policy_changed": [],
        "before_floor": {"claims": 0, "done_tasks": 0},
    }


def violation(rule: int, kind: str = "claim", entity_id: int = 1) -> dict:
    return {
        "rule": rule,
        "name": trail.RULES[rule],
        "entity_kind": kind,
        "entity_id": entity_id,
        "claim_token": None,
        "claimed_entity": None,
        "reason": "because",
    }


def fake_planar(directory: Path, report: dict | str, code: int = 0) -> tuple[Path, Path]:
    """A script that answers `--help` and otherwise logs its call and prints `report`.

    Returns `(script, log)`.
    """
    directory.mkdir(parents=True, exist_ok=True)
    log = directory / "call.log"
    payload = directory / "payload.json"
    payload.write_text(report if isinstance(report, str) else json.dumps(report), encoding="utf-8")
    script = directory / "planar"
    script.write_text(
        "#!/bin/sh\n"
        'case " $* " in *" --help "*) echo "planar health conformance <plan-id>"; exit 0;; esac\n'
        "{\n"
        '  echo "ARGV $*"\n'
        '  echo "HOME $HOME"\n'
        '  echo "PLANAR_DB $PLANAR_DB"\n'
        '  echo "CONFIG $(cat "$PLANAR_CONFIG_PATH")"\n'
        '  echo "DBSIZE $(wc -c < "$PLANAR_DB" | tr -d " ")"\n'
        f"}} > {log}\n"
        f"cat {payload}\n"
        f"exit {code}\n",
        encoding="utf-8",
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script, log


def make_database(path: Path) -> str:
    """A small SQLite file; returns its content hash."""
    connection = sqlite3.connect(path)
    connection.execute("create table t (v text)")
    connection.execute("insert into t values ('a')")
    connection.commit()
    connection.close()
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ReportContractTests(unittest.TestCase):
    def test_valid_report_parses_and_exposes_the_verdict(self) -> None:
        grade = trail.parse_report(json.dumps(sample_report([violation(2)])), 1)
        self.assertFalse(grade.conforms)
        self.assertEqual(grade.violation_keys(), [(2, "claim", 1)])
        clean = trail.parse_report(json.dumps(sample_report()), 0)
        self.assertTrue(clean.conforms)

    def test_each_contract_break_is_refused(self) -> None:
        good = sample_report([violation(1, "task")])

        def broken(mutate) -> str:
            report = json.loads(json.dumps(good))
            mutate(report)
            return json.dumps(report)

        cases = {
            "not json": "{nope",
            "missing key": broken(lambda r: r.pop("overrides")),
            "rule out of range": broken(lambda r: r["violations"][0].update(rule=7)),
            "name does not match rule": broken(lambda r: r["violations"][0].update(name="coder-model")),
            "unknown entity kind": broken(lambda r: r["violations"][0].update(entity_kind="plan")),
            "conforms disagrees": broken(lambda r: r.update(conforms=True)),
            "bad origin": broken(lambda r: r["floor"].update(origin="guess")),
            "non-integer count": broken(lambda r: r["before_floor"].update(claims="1")),
        }
        for label, text in cases.items():
            with self.subTest(label), self.assertRaises(trail.TrailError):
                trail.parse_report(text, 1)

    def test_exit_code_must_agree_with_the_verdict(self) -> None:
        with self.assertRaises(trail.TrailError):
            trail.parse_report(json.dumps(sample_report([violation(2)])), 0)
        with self.assertRaises(trail.TrailError):
            trail.parse_report(json.dumps(sample_report()), 1)

    def test_describe_names_every_violation_and_override(self) -> None:
        grade = trail.parse_report(
            json.dumps(sample_report([violation(2), violation(1, "task", 4)], [{"task_id": 9, "reason": "forced"}])), 1
        )
        text = trail.describe(grade)
        self.assertIn("rule 2 terminal-verb: claim 1", text)
        self.assertIn("rule 1 completed-claim: task 4", text)
        self.assertIn("override: task 9: forced", text)


class ExpectationTests(unittest.TestCase):
    def grade(self, violations: list[dict], overrides: list[dict] | None = None) -> trail.TrailGrade:
        return trail.parse_report(
            json.dumps(sample_report(violations, overrides)), 1 if violations else 0
        )

    def test_exact_match_has_no_failures(self) -> None:
        grade = self.grade([violation(3)], [{"task_id": 2, "reason": "x"}])
        expected = {"conforms": False, "violations": [{"rule": 3, "entity": "claim:1"}], "overrides": [2]}
        self.assertEqual(trail.expectation_failures(grade, expected), [])

    def test_an_extra_or_missing_violation_is_a_failure(self) -> None:
        grade = self.grade([violation(3), violation(4, "claim", 2)])
        expected = {"conforms": False, "violations": [{"rule": 3, "entity": "claim:1"}]}
        self.assertTrue(trail.expectation_failures(grade, expected))
        clean = self.grade([])
        self.assertTrue(trail.expectation_failures(clean, expected))

    def test_overrides_and_before_floor_are_checked_when_declared(self) -> None:
        grade = self.grade([], [{"task_id": 2, "reason": "x"}])
        self.assertTrue(trail.expectation_failures(grade, {"conforms": True, "overrides": []}))
        self.assertTrue(trail.expectation_failures(grade, {"conforms": True, "before_floor": {"claims": 5}}))
        self.assertEqual(trail.expectation_failures(grade, {"conforms": True}), [])


class ScratchIsolationTests(unittest.TestCase):
    def test_scratch_env_roots_every_planar_path_in_the_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            env = trail.scratch_env(directory, {"PATH": "/usr/bin", "HOME": "/Users/real", "PLANAR_DB": "/real.db"})
            trail.assert_scratch(env, directory)
            self.assertEqual(env["PATH"], "/usr/bin")
            self.assertNotIn("/Users/real", env.values())
            self.assertNotIn("/real.db", env.values())

    def test_assert_scratch_refuses_a_path_outside(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            env = trail.scratch_env(directory)
            env["HOME"] = str(Path.home())
            with self.assertRaises(trail.TrailError):
                trail.assert_scratch(env, directory)

    def test_snapshot_copies_without_touching_the_source(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "src.db"
            digest = make_database(source)
            trail.snapshot_database(source, Path(tmp) / "copy" / "dst.db")
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), digest)
            copy = sqlite3.connect(Path(tmp) / "copy" / "dst.db")
            self.assertEqual(copy.execute("select v from t").fetchone(), ("a",))
            copy.close()

    def test_snapshot_refuses_a_missing_database(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(trail.TrailError):
            trail.snapshot_database(Path(tmp) / "absent.db", Path(tmp) / "dst.db")


class RunConformanceTests(unittest.TestCase):
    def test_the_checker_sees_a_scratch_copy_policy_and_flags(self) -> None:
        # The operator's home is a fixture directory outside the temp tree, so
        # the assertion holds when the host's HOME is an ancestor of every
        # tempdir (as in a CI container with HOME=/tmp).
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory(
            prefix="operator-home-fixture-"
        ) as operator_home, mock.patch.dict(os.environ, {"HOME": operator_home}):
            root = Path(tmp)
            source = root / "trail.db"
            digest = make_database(source)
            planar, log = fake_planar(root / "bin", sample_report([violation(2)]), code=1)
            grade = trail.run_conformance(
                source, 7, floor=3, config_text="[policy]\nrequire_model = true\n", scope="global", planar=planar
            )
            seen = log.read_text(encoding="utf-8")
            self.assertEqual(grade.violation_keys(), [(2, "claim", 1)])
            self.assertIn("ARGV health conformance 7 --json --floor 3 --scope global", seen)
            self.assertIn("require_model = true", seen)
            self.assertNotIn(str(source), seen)
            self.assertEqual(str(Path.home()), operator_home)
            self.assertNotIn(operator_home, seen)
            self.assertNotIn("DBSIZE 0", seen)
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), digest)

    def test_floor_flag_is_omitted_when_unset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "trail.db"
            make_database(source)
            planar, log = fake_planar(root / "bin", sample_report())
            trail.run_conformance(source, 1, planar=planar)
            self.assertNotIn("--floor", log.read_text(encoding="utf-8"))

    def test_an_exit_other_than_zero_or_one_is_an_error_not_a_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "trail.db"
            make_database(source)
            planar, _ = fake_planar(root / "bin", "", code=2)
            with self.assertRaises(trail.TrailError):
                trail.run_conformance(source, 1, planar=planar)

    def test_exit_one_without_a_report_is_a_refusal_not_a_verdict(self) -> None:
        # `planar health conformance` exits 1 for a violation AND for a plan, scope or policy it could not read.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "trail.db"
            make_database(source)
            planar, _ = fake_planar(root / "bin", "", code=1)
            with self.assertRaises(trail.TrailError) as caught:
                trail.run_conformance(source, 1, planar=planar)
            self.assertIn("produced no report (exit 1)", str(caught.exception))

    def test_exit_one_with_a_report_is_a_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "trail.db"
            make_database(source)
            planar, _ = fake_planar(root / "bin", sample_report([violation(4)]), code=1)
            self.assertEqual(trail.run_conformance(source, 1, planar=planar).exit_code, 1)

    def test_output_that_is_not_the_report_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "trail.db"
            make_database(source)
            planar, _ = fake_planar(root / "bin", "conformance: plan 1\n", code=0)
            with self.assertRaises(trail.TrailError):
                trail.run_conformance(source, 1, planar=planar)


class ResolvePlanarTests(unittest.TestCase):
    def test_an_explicit_planar_that_cannot_grade_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(trail.TrailUnavailable):
                trail.resolve_planar(Path(tmp), {"PLANAR_EVAL_PLANAR": str(Path(tmp) / "nope"), "PATH": ""})

    def test_the_checkouts_build_wins_over_the_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            built, _ = fake_planar(root / "build" / "debug" / "bin", sample_report())
            installed, _ = fake_planar(root / "installed", sample_report())
            chosen = trail.resolve_planar(root, {"PATH": str(installed.parent)})
            self.assertEqual(chosen, built)

    def test_a_planar_without_the_verb_is_skipped_and_named_when_none_remain(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old = root / "installed" / "planar"
            old.parent.mkdir()
            old.write_text("#!/bin/sh\nexit 64\n", encoding="utf-8")
            old.chmod(old.stat().st_mode | stat.S_IXUSR)
            with self.assertRaises(trail.TrailUnavailable) as caught:
                trail.resolve_planar(root, {"PATH": str(old.parent)})
            self.assertIn("no `health conformance`", str(caught.exception))
            self.assertIn("PLANAR_EVAL_PLANAR", str(caught.exception))


class PolicyTomlTests(unittest.TestCase):
    def test_renders_booleans_and_argv_arrays(self) -> None:
        text = trail.policy_toml({"require_model": True, "task_gate": ["make", "test"], "review_required": False})
        self.assertIn("[policy]", text)
        self.assertIn("require_model = true", text)
        self.assertIn('task_gate = ["make", "test"]', text)
        self.assertIn("review_required = false", text)
        self.assertEqual(trail.policy_toml(None), "")
        self.assertEqual(trail.policy_toml({}), "")

    def test_refuses_a_value_that_is_neither(self) -> None:
        with self.assertRaises(trail.TrailError):
            trail.policy_toml({"require_model": "yes"})


def minimal_spec(**overrides) -> dict:
    spec = {
        "schema_version": 1,
        "id": "t",
        "description": "d",
        "tasks": [{"title": "x", "final": "done"}],
        "claims": [{"task": 1, "status": "completed"}],
        "expected": {"conforms": True},
    }
    spec.update(overrides)
    return spec


class SpecValidationTests(unittest.TestCase):
    def test_a_minimal_spec_is_valid(self) -> None:
        trail.validate_spec(minimal_spec())

    def test_malformed_specs_are_refused(self) -> None:
        bad = {
            "unknown top-level key": minimal_spec(shout=True),
            "wrong schema version": minimal_spec(schema_version=2),
            "no tasks": minimal_spec(tasks=[]),
            "unknown task final": minimal_spec(tasks=[{"title": "x", "final": "finished"}]),
            "claim on a task that does not exist": minimal_spec(claims=[{"task": 2, "status": "completed"}]),
            "unknown claim status": minimal_spec(claims=[{"task": 1, "status": "done"}]),
            "unknown claim key": minimal_spec(claims=[{"task": 1, "status": "completed", "colour": "red"}]),
            "child task without a child plan": minimal_spec(tasks=[{"title": "x", "final": "done", "plan": "child"}]),
            "expected without conforms": minimal_spec(expected={}),
            "expected violation without an entity": minimal_spec(expected={"conforms": False, "violations": [{"rule": 1}]}),
            "unknown policy key": minimal_spec(policy={"after_review": True}),
            "unknown floor": minimal_spec(floor="soon"),
            "review verdict outside the vocabulary": minimal_spec(
                claims=[{"task": 1, "status": "completed", "reviews": [{"session": "r", "verdict": "lgtm"}]}]
            ),
        }
        for label, spec in bad.items():
            with self.subTest(label), self.assertRaises(trail.TrailError):
                trail.validate_spec(spec)

    def test_a_trail_file_must_be_named_for_its_id(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "other-name.json"
            path.write_text(json.dumps(minimal_spec()), encoding="utf-8")
            with self.assertRaises(trail.TrailError):
                trail.load_trail(path)


class ShippedTrailTests(unittest.TestCase):
    def test_every_shipped_trail_is_valid_and_the_corpus_covers_the_checker(self) -> None:
        specs = trail.load_trails()
        self.assertGreaterEqual(len(specs), 16)
        harness.check_trail_corpus(specs)

    def test_the_corpus_check_names_a_rule_no_trail_breaks(self) -> None:
        specs = [(p, s) for p, s in trail.load_trails() if s["id"] != "breaks-queued-gate"]
        with self.assertRaises(harness.EvalFailure) as caught:
            harness.check_trail_corpus(specs)
        self.assertIn("[5]", str(caught.exception))

    def test_the_corpus_check_needs_three_conforming_styles(self) -> None:
        specs = [(p, s) for p, s in trail.load_trails() if s["id"] not in ("conforms-swarm", "conforms-leaderless-swarm")]
        with self.assertRaises(harness.EvalFailure) as caught:
            harness.check_trail_corpus(specs)
        self.assertIn("orchestrator style", str(caught.exception))

    def test_the_grader_never_reads_which_prompt_produced_a_trail(self) -> None:
        source = Path(trail.__file__).read_text(encoding="utf-8")
        for token in ("planar-orchestrator", "ORCHESTRATOR_AGENT", "vendors", "agents/"):
            self.assertNotIn(token, source)

    def test_style_is_a_label_the_grade_does_not_depend_on(self) -> None:
        # The same trail under two different style labels builds the same specification
        # for the checker: `style` is dropped before anything is materialised.
        base = minimal_spec(style="stock session")
        other = minimal_spec(style="swarm")
        trail.validate_spec(base)
        trail.validate_spec(other)
        self.assertEqual({k: v for k, v in base.items() if k != "style"}, {k: v for k, v in other.items() if k != "style"})


class ConformanceExpectationTests(unittest.TestCase):
    def invalid(self, declared) -> str | None:
        messages: list[str] = []

        def record(message: str):
            messages.append(message)
            raise ValueError(message)

        try:
            harness.validate_conformance_expectation(declared, record)
        except ValueError:
            return messages[0]
        return None

    def test_valid_declarations(self) -> None:
        self.assertIsNone(self.invalid(None))
        self.assertIsNone(self.invalid({"conforms": True}))
        self.assertIsNone(self.invalid({"conforms": False, "rules": [2, 4]}))

    def test_invalid_declarations_are_named(self) -> None:
        for declared in (
            {"conforms": "yes"},
            {"conforms": False},
            {"conforms": False, "rules": []},
            {"conforms": False, "rules": [9]},
            {"conforms": False, "rules": [2, 2]},
            {"conforms": True, "rules": [2]},
            {"conforms": True, "extra": 1},
            "conforms",
        ):
            with self.subTest(declared):
                self.assertIsNotNone(self.invalid(declared))

    def test_failures_compare_the_set_of_broken_rules(self) -> None:
        grade = trail.parse_report(json.dumps(sample_report([violation(2), violation(2, "claim", 2), violation(4)])), 1)
        self.assertEqual(harness.conformance_expectation_failures(grade, {"conforms": False, "rules": [2, 4]}), [])
        self.assertTrue(harness.conformance_expectation_failures(grade, {"conforms": False, "rules": [2]}))
        self.assertTrue(harness.conformance_expectation_failures(grade, {"conforms": True}))


def lifecycle_artifacts(
    root: Path, *, artifact_format: int, snapshot: bool, config: str | None = None, plan_id: str = "5"
) -> Path:
    artifacts = root / "artifacts"
    artifacts.mkdir()
    (artifacts / "run.json").write_text(
        json.dumps({"plan_id": plan_id, "artifact_format": artifact_format}), encoding="utf-8"
    )
    if snapshot:
        make_database(artifacts / harness.TRAIL_SNAPSHOT)
    if config is not None:
        (artifacts / harness.TRAIL_CONFIG).write_text(config, encoding="utf-8")
    return artifacts


def trail_options() -> harness.Options:
    return harness.Options(
        mode="lifecycle", vendor="claude", surface="agent", case_filter=None, results_dir=None, keep=True
    )


class GradeArtifactTrailTests(unittest.TestCase):
    def grade(self, artifacts: Path, case: dict, planar: Path | None) -> None:
        env = {"PLANAR_EVAL_PLANAR": str(planar)} if planar else {"PLANAR_EVAL_PLANAR": "/nonexistent/planar"}
        with mock.patch.dict(os.environ, env):
            harness.grade_artifact_trail(case, artifacts, trail_options())

    def test_a_legacy_artifact_set_without_a_snapshot_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            artifacts = lifecycle_artifacts(Path(tmp), artifact_format=2, snapshot=False)
            self.grade(artifacts, {"id": "c"}, None)
            self.assertFalse((artifacts / "conformance.json").exists())

    def test_a_missing_snapshot_is_a_failure_once_the_run_promised_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            artifacts = lifecycle_artifacts(Path(tmp), artifact_format=harness.ARTIFACT_FORMAT_TRAIL, snapshot=False)
            with self.assertRaises(harness.EvalFailure) as caught:
                self.grade(artifacts, {"id": "c"}, None)
            self.assertIn("trail-missing", str(caught.exception))

    def test_a_declared_conforming_trail_with_a_violation_fails_and_names_it(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts = lifecycle_artifacts(root, artifact_format=3, snapshot=True, config="[policy]\nrequire_model = true\n")
            planar, log = fake_planar(root / "bin", sample_report([violation(3)]), code=1)
            case = {"id": "c", "expected": {"conformance": {"conforms": True}}}
            with self.assertRaises(harness.EvalFailure) as caught:
                self.grade(artifacts, case, planar)
            message = str(caught.exception)
            self.assertIn("rule 3 coder-model: claim 1", message)
            self.assertIn("ARGV health conformance 5 --json --floor 1", log.read_text(encoding="utf-8"))
            self.assertIn("require_model = true", log.read_text(encoding="utf-8"))
            self.assertEqual(json.loads((artifacts / "grade.json").read_text())["status"], "fail")

    def test_a_declared_nonconforming_trail_passes_when_the_rules_match(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts = lifecycle_artifacts(root, artifact_format=3, snapshot=True)
            planar, _ = fake_planar(root / "bin", sample_report([violation(2)]), code=1)
            case = {"id": "c", "expected": {"conformance": {"conforms": False, "rules": [2]}}}
            self.grade(artifacts, case, planar)
            recorded = json.loads((artifacts / "conformance.json").read_text(encoding="utf-8"))
            self.assertEqual(recorded["status"], "graded")

    def test_an_undeclared_case_records_the_report_and_does_not_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts = lifecycle_artifacts(root, artifact_format=3, snapshot=True)
            planar, _ = fake_planar(root / "bin", sample_report([violation(4)]), code=1)
            self.grade(artifacts, {"id": "c"}, planar)
            recorded = json.loads((artifacts / "conformance.json").read_text(encoding="utf-8"))
            self.assertEqual(recorded["report"]["violations"][0]["rule"], 4)

    def test_no_planar_fails_a_declared_case_and_only_warns_an_undeclared_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            artifacts = lifecycle_artifacts(Path(tmp), artifact_format=3, snapshot=True)
            with self.assertRaises(harness.EvalFailure) as caught:
                self.grade(artifacts, {"id": "c", "expected": {"conformance": {"conforms": True}}}, None)
            self.assertIn("conformance unavailable", str(caught.exception))
        with tempfile.TemporaryDirectory() as tmp:
            artifacts = lifecycle_artifacts(Path(tmp), artifact_format=3, snapshot=True)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.grade(artifacts, {"id": "c"}, None)
            self.assertIn("conformance not graded", out.getvalue())
            self.assertEqual(
                json.loads((artifacts / "conformance.json").read_text(encoding="utf-8"))["status"], "unavailable"
            )


class OrchestratorPromptTests(unittest.TestCase):
    def test_frontmatter_is_dropped_and_the_body_kept(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "swarm.md"
            path.write_text("---\nname: swarm\ndescription: d\n---\n\nSplit the plan across workers.\n", encoding="utf-8")
            self.assertEqual(harness.orchestrator_prompt_source(path), "Split the plan across workers.\n")
            bare = Path(tmp) / "bare.md"
            bare.write_text("Just do it.\n", encoding="utf-8")
            self.assertEqual(harness.orchestrator_prompt_source(bare), "Just do it.\n")

    def test_a_missing_prompt_file_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(harness.EvalFailure):
            harness.orchestrator_prompt_source(Path(tmp) / "absent.md")

    def test_the_prompt_replaces_the_stock_text_and_the_stock_default_is_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mine.md"
            path.write_text("My orchestrator.\n", encoding="utf-8")
            self.assertEqual(harness.orchestrator_agent_source(prompt=path), "My orchestrator.\n")
            self.assertNotEqual(harness.orchestrator_agent_source(), "My orchestrator.\n")

    def test_parse_args_carries_the_prompt_into_options(self) -> None:
        options, *_ = harness.parse_args(["--lifecycle", "--vendor", "claude", "--orchestrator-prompt", "p.md"])
        self.assertEqual(options.orchestrator_prompt, Path("p.md").resolve())
        options, *_ = harness.parse_args(["--contract-only"])
        self.assertIsNone(options.orchestrator_prompt)


def lifecycle_context(root: Path, database: Path, config: Path) -> harness.LifecycleContext:
    artifacts = root / "artifacts"
    repo = artifacts / "repo"
    repo.mkdir(parents=True)
    return harness.LifecycleContext(
        case_path=root / "case.json",
        case={"id": "wiring-probe"},
        artifacts=artifacts,
        repo=repo,
        env={"PLANAR_DB": str(database), "PLANAR_CONFIG_PATH": str(config), "PATH": os.environ["PATH"]},
        plan_id="5",
        task_id="9",
        task_ids=["9"],
    )


class WiringTests(unittest.TestCase):
    """The call sites: a host run keeps the trail, and grading a lifecycle artifact set runs the check."""

    def test_snapshot_keeps_the_trail_and_the_policy_it_ran_under(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            database = root / "arena.db"
            digest = make_database(database)
            config = root / "config.toml"
            config.write_text("[policy]\nuse_queue = true\n", encoding="utf-8")
            context = lifecycle_context(root, database, config)
            harness.snapshot_arena_trail(context)
            self.assertTrue((context.artifacts / harness.TRAIL_SNAPSHOT).is_file())
            self.assertEqual(
                (context.artifacts / harness.TRAIL_CONFIG).read_text(encoding="utf-8"), "[policy]\nuse_queue = true\n"
            )
            self.assertEqual(hashlib.sha256(database.read_bytes()).hexdigest(), digest)

    def collect(self, vendor: str) -> bool:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            database = root / "arena.db"
            make_database(database)
            config = root / "config.toml"
            config.write_text("", encoding="utf-8")
            context = lifecycle_context(root, database, config)
            options = harness.Options(
                mode="lifecycle", vendor=vendor, surface="agent", case_filter=None, results_dir=None, keep=True
            )
            quiet = mock.patch.multiple(
                harness,
                run_json=mock.Mock(return_value={"agent_activity": {"claims": []}, "active": []}),
                read_observed_records=mock.Mock(return_value=[]),
                assert_no_recording_failures=mock.Mock(),
                assert_wrapper_not_bypassed=mock.Mock(),
                assert_terminal_verb_lifecycle=mock.Mock(return_value=[]),
                write_lifecycle_warnings=mock.Mock(),
                merge_lifecycle_events=mock.Mock(return_value=[]),
            )
            stop = mock.patch.object(harness.queue_observation, "submit", side_effect=ValueError("stop here"))
            with quiet, stop, self.assertRaises(harness.EvalFailure):
                harness.collect_lifecycle_artifacts(context, options)
            return (context.artifacts / harness.TRAIL_SNAPSHOT).is_file()

    def test_a_host_run_snapshots_the_trail_and_a_fixture_replay_does_not(self) -> None:
        self.assertTrue(self.collect("claude"))
        self.assertFalse(self.collect(""))

    def test_grading_a_lifecycle_artifact_set_runs_the_conformance_check(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            test_harness.write_lifecycle_artifacts(artifacts)
            (artifacts / "run.json").write_text(json.dumps({"plan_id": "5", "artifact_format": 1}), encoding="utf-8")
            make_database(artifacts / harness.TRAIL_SNAPSHOT)
            planar, _ = fake_planar(root / "bin", sample_report([violation(4)]), code=1)
            case = test_harness.lifecycle_case()
            case["expected"]["conformance"] = {"conforms": True}  # type: ignore[index]
            with mock.patch.dict(os.environ, {"PLANAR_EVAL_PLANAR": str(planar)}):
                with self.assertRaises(harness.EvalFailure) as caught:
                    harness.grade_lifecycle_artifacts(case, artifacts, test_harness.lifecycle_options())
            self.assertIn("rule 4 parent-action", str(caught.exception))
            # The same artifact set passes when the trail is what the case declares.
            case["expected"]["conformance"] = {"conforms": False, "rules": [4]}  # type: ignore[index]
            with mock.patch.dict(os.environ, {"PLANAR_EVAL_PLANAR": str(planar)}):
                harness.grade_lifecycle_artifacts(case, artifacts, test_harness.lifecycle_options())


class PrepareTests(unittest.TestCase):
    """`prepare_lifecycle_fixture` stages any orchestrator prompt and puts the arena under the case's policy."""

    def prepare(self, tmp: str, prompt: Path | None, policy: dict | None) -> harness.LifecycleContext:
        case = {
            "id": "prompt-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "setup": {"fixture": "foreign-flat", "specialist_scenario": "success"},
            "lifecycle": {"policy": policy} if policy else {},
        }
        options = harness.Options(
            mode="lifecycle-fixture",
            vendor="",
            surface="agent",
            case_filter=None,
            results_dir=Path(tmp),
            keep=True,
            orchestrator_prompt=prompt,
        )
        return harness.prepare_lifecycle_fixture(
            harness.ROOT / "evals" / "orchestrator" / "cases" / "foreign-flat-lifecycle-success.json", case, options, "fixture"
        )

    def test_a_named_prompt_is_staged_in_place_of_the_stock_orchestrator(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prompt = Path(tmp) / "swarm.md"
            prompt.write_text("---\nname: swarm\n---\n\nSplit the plan across peers.\n", encoding="utf-8")
            context = self.prepare(tmp, prompt, None)
            staged = (context.repo / ".claude" / "agents" / "planar-orchestrator.md").read_text(encoding="utf-8")
            self.assertIn("Split the plan across peers.", staged)
            self.assertNotIn("name: swarm", staged)
            instructions = (context.repo / ".eval" / "orchestrator.instructions.md").read_text(encoding="utf-8")
            self.assertIn("Split the plan across peers.", instructions)
            self.assertFalse((context.repo / ".claude" / "agents" / "doctrine.md").exists())
            recorded = json.loads((context.artifacts / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(recorded["orchestrator_prompt"]["sha256"], hashlib.sha256(prompt.read_bytes()).hexdigest())

    def test_the_stock_orchestrator_is_still_staged_without_a_prompt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            context = self.prepare(tmp, None, None)
            staged = (context.repo / ".claude" / "agents" / "planar-orchestrator.md").read_bytes()
            stock = harness.ROOT / "agents" / "planar-orchestrator.md"
            if stock.is_file():
                self.assertEqual(staged, stock.read_bytes())
            recorded = json.loads((context.artifacts / "run.json").read_text(encoding="utf-8"))
            self.assertIsNone(recorded["orchestrator_prompt"])

    def test_the_cases_policy_is_written_to_the_arena_config(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            context = self.prepare(tmp, None, {"require_model": True, "use_queue": True, "task_gate": ["make", "test"]})
            config = Path(context.env["PLANAR_CONFIG_PATH"]).read_text(encoding="utf-8")
            self.assertIn("[policy]", config)
            self.assertIn("require_model = true", config)
            self.assertIn('task_gate = ["make", "test"]', config)


class GradeTrailCommandTests(unittest.TestCase):
    def run_command(self, arguments: list[str], planar: Path) -> tuple[int, str]:
        out = io.StringIO()
        with mock.patch.dict(os.environ, {"PLANAR_EVAL_PLANAR": str(planar)}), contextlib.redirect_stdout(out):
            code = harness.main(arguments)
        return code, out.getvalue()

    def test_exit_code_follows_the_checker_and_the_source_is_untouched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            database = root / "some.db"
            digest = make_database(database)
            bad, _ = fake_planar(root / "bad", sample_report([violation(2)]), code=1)
            code, text = self.run_command(["--grade-trail", str(database), "--plan", "3"], bad)
            self.assertEqual(code, 1)
            self.assertIn("rule 2 terminal-verb: claim 1", text)
            good, _ = fake_planar(root / "good", sample_report(), code=0)
            code, text = self.run_command(["--grade-trail", str(database), "--plan", "3", "--json"], good)
            self.assertEqual(code, 0)
            self.assertTrue(json.loads(text)["conforms"])
            self.assertEqual(hashlib.sha256(database.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
