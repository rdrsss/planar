#!/usr/bin/env python3
"""Offline tests for the deterministic paths of the planning eval harness."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Appended, not inserted at 0: both directories have a `harness.py`, and
# `import harness` here must resolve to THIS harness, not the orchestrator's.
sys.path.append(str(Path(__file__).resolve().parents[1] / "orchestrator"))

import harness  # noqa: E402
import arena  # noqa: E402

HARNESS_PATH = Path(__file__).resolve().parent / "harness.py"


def run_harness_main(argv: list[str]) -> tuple[int, str, str]:
    """Run `harness.main()` in-process, capturing stdout/stderr and the exit code.

    Shared by every test class that needs to drive the CALLER (`main()`),
    not just a function it calls — the only way to prove a check is wired
    in at its call site rather than merely correct in isolation.
    """
    import io
    import contextlib

    old_argv = sys.argv
    sys.argv = ["harness.py", *argv]
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = harness.main()
    finally:
        sys.argv = old_argv
    return code, out.getvalue(), err.getvalue()


class CaseLoadingTests(unittest.TestCase):
    def test_unknown_case_names_the_available_ones(self) -> None:
        # Silently doing nothing on a typo is how a green run comes to mean
        # "nothing ran".
        with self.assertRaises(harness.ConfigError) as ctx:
            harness.load_case("no-such-case")
        self.assertIn("spec-draft-quality", str(ctx.exception))

    def test_shipped_case_loads_with_its_assets(self) -> None:
        case = harness.load_case("spec-draft-quality")
        self.assertGreaterEqual(int(case["trials"]), 2, "one sample cannot show variance")
        self.assertTrue(harness.read_asset(case, "goal_fixture").strip())
        self.assertTrue(harness.read_asset(case, "grader").strip())
        self.assertIn("v1", case["grader_version"])

    def test_goal_fixture_is_not_reachable_as_committed_spec_text(self) -> None:
        # The goal must be supplied at run time, not discoverable as an
        # existing answer the model could pattern-match.
        case = harness.load_case("spec-draft-quality")
        goal = harness.read_asset(case, "goal_fixture")
        self.assertNotIn("acceptance criteria", goal.lower())


class StructuralGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = harness.load_case("spec-draft-quality")["structural"]

    def test_complete_draft_passes(self) -> None:
        draft = (
            "product-spec tech-spec test-spec roadmap\n"
            "Goal: x\nNon-goals: y\nAcceptance: z\nOpen questions: w\n"
        )
        self.assertTrue(harness.check_structure(draft, self.spec).ok)

    def test_missing_pieces_are_named_not_just_counted(self) -> None:
        result = harness.check_structure("Goal: x", self.spec)
        self.assertFalse(result.ok)
        self.assertIn("tech-spec", result.missing_artifacts)
        self.assertIn("Acceptance", result.missing_sections)
        self.assertTrue(result.reasons())

    def test_unsupported_claims_fail_even_when_structure_is_complete(self) -> None:
        # Strict preview: a draft that claims work it did not do reads as
        # progress, which is worse than an obvious gap.
        draft = (
            "product-spec tech-spec test-spec roadmap\n"
            "Goal: x\nNon-goals: y\nAcceptance: all tests pass\nOpen questions: w\n"
        )
        result = harness.check_structure(draft, self.spec)
        self.assertFalse(result.ok)
        self.assertIn("all tests pass", result.forbidden_found)


class GraderOutputTests(unittest.TestCase):
    def test_valid_output_parses(self) -> None:
        scores = harness.parse_grader_output('{"scores": {"a": 0.5, "b": 1}}')
        self.assertEqual(scores, {"a": 0.5, "b": 1.0})

    def test_prose_wrapped_json_still_parses(self) -> None:
        scores = harness.parse_grader_output('Here you go:\n{"scores": {"a": 0.25}}\nHope that helps.')
        self.assertEqual(scores, {"a": 0.25})

    def test_unparseable_output_raises_rather_than_scoring_zero(self) -> None:
        # A zero is a VERDICT. "Did not grade" is not a verdict, and conflating
        # them makes a broken grader look like a consistently bad model.
        for bad in ("", "no json here", "{not json}", '{"scores": {}}', '{"nope": 1}'):
            with self.assertRaises(harness.GraderError):
                harness.parse_grader_output(bad)

    def test_out_of_range_and_non_numeric_scores_are_rejected(self) -> None:
        for bad in ('{"scores": {"a": 1.4}}', '{"scores": {"a": -0.1}}', '{"scores": {"a": "high"}}'):
            with self.assertRaises(harness.GraderError):
                harness.parse_grader_output(bad)


class VarianceTests(unittest.TestCase):
    @staticmethod
    def _trial(i: int, score: float) -> harness.TrialResult:
        return harness.TrialResult(i, score, {}, harness.StructuralResult(ok=True))

    def test_spread_is_reported_not_averaged_away(self) -> None:
        stable = harness.aggregate([self._trial(1, 0.8), self._trial(2, 0.8), self._trial(3, 0.8)])
        swingy = harness.aggregate([self._trial(1, 1.0), self._trial(2, 0.4), self._trial(3, 1.0)])
        # Same-ish mean, very different reliability — the operator must be able
        # to tell these apart before routing work to one of them.
        self.assertAlmostEqual(stable["stdev"], 0.0)
        self.assertGreater(swingy["stdev"], 0.3)
        self.assertEqual(swingy["min"], 0.4)

    def test_single_trial_reports_zero_stdev_without_erroring(self) -> None:
        self.assertEqual(harness.aggregate([self._trial(1, 0.9)])["stdev"], 0.0)


class SeededRecallTests(unittest.TestCase):
    WEIGHTS = {"critical": 8, "high": 4, "medium": 2, "low": 1}
    MANIFEST = [
        {"id": "crit", "severity": "critical", "summary": "s"},
        {"id": "hi", "severity": "high", "summary": "s"},
        {"id": "lo1", "severity": "low", "summary": "s"},
        {"id": "lo2", "severity": "low", "summary": "s"},
    ]

    def test_severity_weighting_punishes_a_critical_miss(self) -> None:
        # Finding three of four defects looks good unweighted (0.75) but must
        # not, when the missed one is the critical: an unweighted score lets a
        # reviewer pass by sweeping up the easy majority.
        easy, _ = harness.severity_recall(self.MANIFEST, ["hi", "lo1", "lo2"], self.WEIGHTS)
        only_crit, _ = harness.severity_recall(self.MANIFEST, ["crit"], self.WEIGHTS)
        self.assertLess(easy, 0.5)
        self.assertGreater(only_crit, easy)

    def test_missed_ids_are_named(self) -> None:
        _, missed = harness.severity_recall(self.MANIFEST, ["crit"], self.WEIGHTS)
        self.assertEqual(sorted(missed), ["hi", "lo1", "lo2"])

    def test_shotgun_review_fails_precision_despite_perfect_recall(self) -> None:
        result = harness.score_seeded_recall(
            self.MANIFEST,
            {"matched": [d["id"] for d in self.MANIFEST], "unsupported_count": 30, "total_findings": 34},
            self.WEIGHTS,
        )
        self.assertEqual(result.recall, 1.0)
        self.assertLess(result.precision, 0.5)

    def test_focused_review_passes_both(self) -> None:
        result = harness.score_seeded_recall(
            self.MANIFEST,
            {"matched": ["crit", "hi"], "unsupported_count": 1, "total_findings": 3},
            self.WEIGHTS,
        )
        self.assertGreater(result.recall, 0.6)
        self.assertGreater(result.precision, 0.6)

    def test_empty_review_scores_zero_precision_not_perfect(self) -> None:
        # Zero findings means no evidence of precision, not flawless precision.
        self.assertEqual(harness.precision(0, 0), 0.0)

    def test_grader_inventing_a_defect_id_is_an_error(self) -> None:
        # Trusting an unknown id would silently inflate recall.
        with self.assertRaises(harness.GraderError):
            harness.score_seeded_recall(
                self.MANIFEST,
                {"matched": ["crit", "not-a-real-defect"], "unsupported_count": 0, "total_findings": 2},
                self.WEIGHTS,
            )

    def test_incoherent_grader_counts_are_an_error(self) -> None:
        with self.assertRaises(harness.GraderError):
            harness.score_seeded_recall(
                self.MANIFEST,
                {"matched": [], "unsupported_count": 9, "total_findings": 2},
                self.WEIGHTS,
            )

    def test_recall_grader_output_must_be_complete(self) -> None:
        for bad in ("", "no json", '{"matched": []}', '{"matched": "crit", "unsupported_count": 0, "total_findings": 1}'):
            with self.assertRaises(harness.GraderError):
                harness.parse_recall_grader(bad)

    def test_shipped_manifest_loads_and_is_severity_diverse(self) -> None:
        case = harness.load_case("spec-review-seeded-recall")
        manifest = harness.load_manifest(case)
        severities = {d["severity"] for d in manifest}
        self.assertIn("critical", severities)
        self.assertGreater(len(severities), 2, "a single-severity manifest cannot test weighting")


class SentinelLineageTests(unittest.TestCase):
    ENTITIES = [
        {"kind": "decision", "slot": "locked-decision", "must_carry_digest": True},
        {"kind": "question", "slot": "open-question", "must_carry_digest": False},
    ]

    def _sentinels(self, run: str = "r1") -> list[harness.Sentinel]:
        return harness.mint_sentinels(self.ENTITIES, run)

    def test_sentinels_are_unique_per_run(self) -> None:
        # A stale row from a previous run must not read as a fresh pass.
        first = {s.value for s in self._sentinels("r1")}
        second = {s.value for s in self._sentinels("r2")}
        self.assertEqual(len(first), len(self.ENTITIES))
        self.assertFalse(first & second)

    def test_correct_placement_passes(self) -> None:
        sentinels = self._sentinels()
        observed = {s.kind: [s.value] for s in sentinels}
        digests = {s.slot: "abc" for s in sentinels}
        self.assertTrue(harness.check_lineage(sentinels, observed, digests, {"locked-decision"}).ok)

    def test_misplaced_sentinel_fails_even_though_the_text_exists(self) -> None:
        # "The text is in the database somewhere" is not the property.
        sentinels = self._sentinels()
        observed = {"question": [s.value for s in sentinels]}
        result = harness.check_lineage(sentinels, observed)
        self.assertFalse(result.ok)
        self.assertTrue(result.misplaced)

    def test_missing_sentinel_is_named(self) -> None:
        sentinels = self._sentinels()
        result = harness.check_lineage(sentinels, {"decision": [sentinels[0].value]})
        self.assertIn("open-question", result.missing)

    def test_duplicate_arrival_fails_replay_idempotence(self) -> None:
        # Ingesting twice must not duplicate entities.
        sentinels = self._sentinels()
        observed = {s.kind: [s.value] for s in sentinels}
        observed["decision"] = [sentinels[0].value, sentinels[0].value]
        self.assertFalse(harness.check_lineage(sentinels, observed).ok)

    def test_missing_digest_fails_when_required(self) -> None:
        # Lineage proven at the first hop only is not lineage.
        sentinels = self._sentinels()
        observed = {s.kind: [s.value] for s in sentinels}
        result = harness.check_lineage(sentinels, observed, {}, {"locked-decision"})
        self.assertFalse(result.ok)
        self.assertIn("locked-decision", result.missing_digest)

    def test_diagnostics_keep_sentinels_and_drop_source_bodies(self) -> None:
        sentinels = self._sentinels()
        body = "a confidential paragraph from the source document"
        out = harness.redact_diagnostics(f"{sentinels[0].value} near {body}", sentinels, [body])
        self.assertIn(sentinels[0].value, out)
        self.assertNotIn("confidential paragraph", out)

    def test_shares_the_arena_isolation_assertion(self) -> None:
        # The exact accident that migrated a live database during plan 950,
        # now caught by the same assertion the orchestrator harness uses.
        with tempfile.TemporaryDirectory(prefix="planar-eval-planning-test-") as tmp:
            root = Path(tmp)
            env = harness.build_host_env(root)
            arena.assert_isolated(env, root)  # a fresh arena passes

            leaked = dict(env)
            leaked["PLANAR_DB"] = str(Path.home() / ".planar" / "planar.db")
            with self.assertRaises(arena.ArenaIsolationError) as ctx:
                arena.assert_isolated(leaked, root)
            self.assertIn("PLANAR_DB", str(ctx.exception))


class DryRunEndToEndTests(unittest.TestCase):
    """Test-spec 621, scenario 3 (edge): the dry-run CLI path, for real.

    Every other isolation test in this file calls `build_host_env` /
    `assert_isolated` directly, in-process. That proves the functions work
    but never proves `harness.py --dry-run` actually reaches them: a
    mistake in `main()`'s dry-run branch (wrong case kind, an exception
    swallowed before the isolation check runs, exit code wired to the
    wrong constant) would pass every other test in this file while the
    real CLI invocation silently skipped the isolation self-check. This
    launches the harness as the subprocess a user or CI actually runs.
    """

    def test_dry_run_subprocess_exits_pass_and_proves_isolation(self) -> None:
        result = subprocess.run(
            [sys.executable, str(HARNESS_PATH), "--case", "spec-draft-quality", "--dry-run"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(
            result.returncode,
            harness.EXIT_PASS,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        self.assertIn("isolation", result.stdout.lower())
        self.assertIn("dry-run OK", result.stdout)

    def test_dry_run_subprocess_fails_on_an_unknown_case(self) -> None:
        # The negative half: the same subprocess path must exit non-zero
        # (config error) rather than exiting 0 on a typo.
        result = subprocess.run(
            [sys.executable, str(HARNESS_PATH), "--case", "no-such-case", "--dry-run"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, harness.EXIT_CONFIG)


class PacketReadinessTests(unittest.TestCase):
    FIELDS = ["title", "acceptance_criteria", "next_action"]
    MARKERS = ["is implemented and tested", "implement per acceptance criteria", "todo"]

    def _packet(self, **over: str) -> dict[str, str]:
        base = {
            "title": "Add lease renewal",
            "acceptance_criteria": "renew extends lease_expires_at by the configured TTL",
            "next_action": "Read src/engine/lease.zig renew(), add the guard",
        }
        base.update(over)
        return base

    def test_a_real_packet_is_ready(self) -> None:
        self.assertTrue(harness.check_packet_readiness(self._packet(), self.FIELDS, self.MARKERS).ready)

    def test_generic_acceptance_is_rejected_not_noted(self) -> None:
        # This is the exact placeholder several tasks in this very plan shipped
        # with; dispatching against it yields an unjudgeable run.
        verdict = harness.check_packet_readiness(
            self._packet(acceptance_criteria="The feature is implemented and tested."),
            self.FIELDS,
            self.MARKERS,
        )
        self.assertFalse(verdict.ready)
        self.assertTrue(any("generic acceptance_criteria" in r for r in verdict.reasons))

    def test_verdict_names_which_field_failed(self) -> None:
        # A bare boolean would be useless: the remedy differs per field.
        verdict = harness.check_packet_readiness({"title": "x"}, self.FIELDS, self.MARKERS)
        self.assertFalse(verdict.ready)
        joined = " ".join(verdict.reasons)
        self.assertIn("acceptance_criteria", joined)
        self.assertIn("next_action", joined)

    def test_blank_field_counts_as_missing(self) -> None:
        verdict = harness.check_packet_readiness(self._packet(next_action="   "), self.FIELDS, self.MARKERS)
        self.assertFalse(verdict.ready)

    def test_stale_digest_is_refused(self) -> None:
        # Running here would dispatch against state the operator never saw.
        self.assertFalse(harness.check_digest_freshness("abc", "xyz").ready)
        self.assertTrue(harness.check_digest_freshness("abc", "abc").ready)

    def test_absent_digest_is_not_treated_as_fresh(self) -> None:
        # Unknown is not the same as unchanged.
        self.assertFalse(harness.check_digest_freshness("", "abc").ready)
        self.assertFalse(harness.check_digest_freshness("abc", "").ready)

    def test_stranded_claim_after_cancellation_fails(self) -> None:
        active = [{"entity_kind": "task", "entity_id": 7, "status": "active"}]
        self.assertFalse(harness.check_no_claim_leak(active, 7).ready)
        released = [{"entity_kind": "task", "entity_id": 7, "status": "released"}]
        self.assertTrue(harness.check_no_claim_leak(released, 7).ready)

    def test_another_tasks_claim_is_not_our_leak(self) -> None:
        other = [{"entity_kind": "task", "entity_id": 99, "status": "active"}]
        self.assertTrue(harness.check_no_claim_leak(other, 7).ready)


class BriefCompletenessTests(unittest.TestCase):
    MANDATORY = [
        "task_title", "acceptance_criteria", "spec_citations", "locked_decisions",
        "scenarios", "dependencies", "validation_gates", "packet_digest",
    ]

    def _source(self) -> dict[str, str]:
        return {e: f"value-for-{e}" for e in self.MANDATORY}

    def test_faithful_brief_passes(self) -> None:
        src = self._source()
        self.assertTrue(harness.check_brief_completeness(dict(src), src, self.MANDATORY).ok)

    def test_missing_element_fails_and_is_named(self) -> None:
        # Partial context is worse than none: the coder cannot tell it is
        # missing and proceeds confidently.
        src = self._source()
        brief = {k: v for k, v in src.items() if k != "spec_citations"}
        verdict = harness.check_brief_completeness(brief, src, self.MANDATORY)
        self.assertFalse(verdict.ok)
        self.assertIn("spec_citations", verdict.missing)

    def test_empty_value_counts_as_missing(self) -> None:
        src = self._source()
        for empty in ("", [], {}, None):
            brief = dict(src, scenarios=empty)
            self.assertIn("scenarios", harness.check_brief_completeness(brief, src, self.MANDATORY).missing)

    def test_silently_altered_element_fails(self) -> None:
        # The dangerous case: it LOOKS complete, so nothing downstream notices
        # the coder is working from a rewritten acceptance criterion.
        src = self._source()
        brief = dict(src, acceptance_criteria="quietly rewritten")
        verdict = harness.check_brief_completeness(brief, src, self.MANDATORY)
        self.assertFalse(verdict.ok)
        self.assertIn("acceptance_criteria", verdict.altered)

    def test_every_problem_is_reported_not_just_the_first(self) -> None:
        src = self._source()
        brief = dict(src, acceptance_criteria="changed")
        del brief["dependencies"]
        verdict = harness.check_brief_completeness(brief, src, self.MANDATORY)
        self.assertIn("dependencies", verdict.missing)
        self.assertIn("acceptance_criteria", verdict.altered)
        self.assertGreaterEqual(len(verdict.reasons()), 2)

    def test_shell_punctuation_is_content_not_a_threat(self) -> None:
        # Briefs carry operator prose; rejecting backticks or $() would break
        # real briefs. Same rule the routing surfaces apply to opaque ids.
        src = self._source()
        src["acceptance_criteria"] = "reject `--opaque; $(whoami) && rm -rf /` verbatim"
        self.assertTrue(harness.check_brief_completeness(dict(src), src, self.MANDATORY).ok)

    def test_control_characters_are_refused(self) -> None:
        src = self._source()
        src["scenarios"] = "line\x00truncated"
        verdict = harness.check_brief_completeness(dict(src), src, self.MANDATORY)
        self.assertFalse(verdict.ok)
        self.assertIn("scenarios", verdict.unsafe)

    def test_newlines_and_tabs_remain_legal(self) -> None:
        src = self._source()
        src["acceptance_criteria"] = "- one\n- two\tindented"
        self.assertTrue(harness.check_brief_completeness(dict(src), src, self.MANDATORY).ok)

    def test_packet_digest_is_mandatory(self) -> None:
        # Ties the brief to the state it was built from, so one that outlived
        # its packet is detectable.
        self.assertIn("packet_digest", self.MANDATORY)
        src = self._source()
        brief = {k: v for k, v in src.items() if k != "packet_digest"}
        self.assertIn("packet_digest", harness.check_brief_completeness(brief, src, self.MANDATORY).missing)


class HostArgvTests(unittest.TestCase):
    def test_model_is_a_separate_argv_element(self) -> None:
        argv = harness.host_argv("claude-opus-5")
        self.assertIn("--model", argv)
        self.assertEqual(argv[argv.index("--model") + 1], "claude-opus-5")

    def test_opaque_id_is_never_shell_syntax(self) -> None:
        hostile = "--opaque;$(whoami)"
        argv = harness.host_argv(hostile)
        self.assertEqual(argv[argv.index("--model") + 1], hostile)

    def test_absent_model_omits_the_flag(self) -> None:
        self.assertNotIn("--model", harness.host_argv(None))


class LiveHostAuthBeforeSpawnTests(unittest.TestCase):
    """Planar artifact 626 / task 6872: staging is not authenticating.

    This harness always spawns `claude` (`host_argv`). Keychain-backed
    `claude` login does not follow into a scratch `CLAUDE_CONFIG_DIR`, so
    the live trial loop in `main()` must call `arena.assert_vendor_auth`
    for "claude" before `run_model` ever spawns the host — and must NOT
    do so on the `--dry-run` path, which never spawns a host at all.
    """

    _run_main = staticmethod(run_harness_main)

    def test_live_loop_invokes_assert_vendor_auth_before_run_model(self) -> None:
        # Call-site mutant-kill evidence: a no-op or deleted
        # assert_vendor_auth call at this call site would never raise
        # here, run_model would be reached, and this test would fail on
        # both the exit-code and the not-called assertions below.
        call_order: list[str] = []

        def _auth_side_effect(env, vendor):
            call_order.append(f"auth:{vendor}")
            raise arena.VendorAuthError("no usable auth (test sentinel)")

        run_model_mock = mock.Mock(side_effect=lambda *a, **k: call_order.append("run_model"))
        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "stage_vendor_config"), \
                mock.patch.object(harness.arena, "assert_vendor_auth", side_effect=_auth_side_effect), \
                mock.patch.object(harness, "run_model", run_model_mock):
            code, out, err = self._run_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_FAIL)
        self.assertIn("no usable auth", out + err)
        run_model_mock.assert_not_called()
        self.assertEqual(call_order, ["auth:claude"])

    def test_live_loop_fails_closed_with_a_real_missing_token_env(self) -> None:
        # End-to-end with the REAL assert_vendor_auth: an arena env built
        # from a base_env carrying no claude auth surface must refuse
        # before run_model spawns anything.
        real_make_arena = arena.make_arena
        scrubbed_base_env = {"PATH": __import__("os").environ.get("PATH", "")}

        def scrubbed_make_arena(root, base_env=None):
            return real_make_arena(root, base_env=scrubbed_base_env)

        run_model_mock = mock.Mock()
        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "make_arena", side_effect=scrubbed_make_arena), \
                mock.patch.object(harness.arena, "stage_vendor_config"), \
                mock.patch.object(harness, "run_model", run_model_mock):
            code, out, err = self._run_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_FAIL)
        combined = out + err
        self.assertIn("CLAUDE_CODE_OAUTH_TOKEN", combined)
        self.assertIn("ANTHROPIC_API_KEY", combined)
        run_model_mock.assert_not_called()

    def test_dry_run_never_calls_assert_vendor_auth(self) -> None:
        with mock.patch.object(harness.arena, "assert_vendor_auth") as auth_mock:
            code, _out, _err = self._run_main(
                ["--case", "spec-draft-quality", "--dry-run"]
            )
        self.assertEqual(code, harness.EXIT_PASS)
        auth_mock.assert_not_called()


class LiveKindDispatchTests(unittest.TestCase):
    """Test-spec 621, scenario 3014 / task hh-planning-kind-dispatch.

    Four of the five shipped kinds have no live grading path. Before the
    fix these fell through the trial loop, paid for a host call, then
    raised `KeyError: 'structural'` — a key only `draft-quality` cases
    declare. `assert_live_path` refuses these BEFORE any provider call;
    `--dry-run` must keep working for all five.
    """

    NO_LIVE_PATH_CASES = (
        "ingest-sentinel-lineage",
        "pulled-task-readiness",
        "coder-brief-completeness",
        "spec-review-seeded-recall",
    )

    _run_main = staticmethod(run_harness_main)

    def test_assert_live_path_accepts_draft_quality(self) -> None:
        harness.assert_live_path(harness.load_case("spec-draft-quality"))  # must not raise

    def test_assert_live_path_refuses_every_other_kind_and_names_it(self) -> None:
        for case_id in self.NO_LIVE_PATH_CASES:
            case = harness.load_case(case_id)
            with self.subTest(case=case_id):
                with self.assertRaises(harness.NoLiveGraderError) as ctx:
                    harness.assert_live_path(case)
                self.assertIsInstance(ctx.exception, harness.ConfigError)
                self.assertIn(case["kind"], str(ctx.exception))
                self.assertIn("draft-quality", str(ctx.exception))

    def test_live_dispatch_refuses_before_any_provider_call_for_each_kind(self) -> None:
        # Call-site mutant-kill evidence: deleting or no-opping the
        # `assert_live_path(case)` call in `main()` would let these four
        # cases fall into the trial loop and reach `run_model` /
        # `arena.make_arena` — this test observes both directly and would
        # fail on the exit code, the message, and the not-called
        # assertions below.
        for case_id in self.NO_LIVE_PATH_CASES:
            with self.subTest(case=case_id):
                case = harness.load_case(case_id)
                run_model_mock = mock.Mock()
                make_arena_mock = mock.Mock(side_effect=arena.make_arena)
                with mock.patch.object(harness, "run_model", run_model_mock), \
                        mock.patch.object(harness.arena, "make_arena", make_arena_mock):
                    code, out, err = self._run_main(["--case", case_id])
                combined = out + err
                self.assertEqual(code, harness.EXIT_CONFIG, f"stdout:\n{out}\nstderr:\n{err}")
                self.assertIn(case["kind"], combined)
                self.assertIn("draft-quality", combined)
                run_model_mock.assert_not_called()
                make_arena_mock.assert_not_called()

    def test_dry_run_still_works_for_every_kind_without_the_live_guard(self) -> None:
        for case_id in ("spec-draft-quality", *self.NO_LIVE_PATH_CASES):
            with self.subTest(case=case_id):
                code, out, err = self._run_main(["--case", case_id, "--dry-run"])
                self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")


class GraderModelValidationTests(unittest.TestCase):
    """Test-spec 621, scenario 3015 / task hh-planning-grader-split.

    `grader_model` is a required `draft-quality` case field, validated to
    differ from the drafter (`--model`) — a case whose grader model equals
    the drafter is refused at validation, not silently allowed to grade
    itself.
    """

    def test_shipped_case_declares_a_grader_model(self) -> None:
        case = harness.load_case("spec-draft-quality")
        self.assertTrue(case["grader_model"])

    def test_missing_grader_model_fails_case_load(self) -> None:
        # Write a scratch copy of the shipped draft-quality case with
        # `grader_model` removed, and prove `load_case` refuses it by the
        # same required-key gate every other draft-quality key uses.
        import json as _json

        case_dir = Path(harness.EVAL_ROOT) / "cases"
        original = _json.loads((case_dir / "spec-draft-quality.json").read_text(encoding="utf-8"))
        mutated = dict(original)
        del mutated["grader_model"]
        scratch_id = "spec-draft-quality-missing-grader-model-scratch"
        scratch_path = case_dir / f"{scratch_id}.json"
        scratch_path.write_text(_json.dumps(mutated), encoding="utf-8")
        try:
            with self.assertRaises(harness.ConfigError) as ctx:
                harness.load_case(scratch_id)
            self.assertIn("grader_model", str(ctx.exception))
        finally:
            scratch_path.unlink()

    def test_distinct_grader_model_is_accepted(self) -> None:
        case = harness.load_case("spec-draft-quality")
        harness.assert_grader_differs_from_drafter(case, "claude-sonnet-5")  # must not raise
        self.assertNotEqual(case["grader_model"], "claude-sonnet-5")

    def test_equal_grader_and_drafter_models_are_refused(self) -> None:
        case = harness.load_case("spec-draft-quality")
        with self.assertRaises(harness.ConfigError) as ctx:
            harness.assert_grader_differs_from_drafter(case, case["grader_model"])
        self.assertIn(case["grader_model"], str(ctx.exception))

    def test_unspecified_drafter_model_does_not_raise_the_equality_check(self) -> None:
        # `assert_grader_differs_from_drafter` alone tolerates an unspecified
        # drafter — it has nothing concrete to compare `grader_model`
        # against. This is exercised ONLY on the dry-run path in practice:
        # `assert_live_drafter_model_required` (see `LiveDrafterModelRequiredTests`
        # below) refuses an unspecified drafter on every LIVE draft-quality
        # run before this function is ever reached, so the two functions
        # together — not this one alone — are what make the split validated
        # rather than assumed for a live run.
        case = harness.load_case("spec-draft-quality")
        harness.assert_grader_differs_from_drafter(case, None)

    def test_non_draft_kinds_have_no_grader_model_requirement(self) -> None:
        case = harness.load_case("coder-brief-completeness")
        self.assertNotIn("grader_model", case)
        harness.assert_grader_differs_from_drafter(case, "anything")  # no-op, must not raise

    def test_main_refuses_when_model_flag_equals_grader_model(self) -> None:
        # Call-site mutant-kill evidence: deleting the
        # `assert_grader_differs_from_drafter(case, args.model)` call in
        # `main()` would let this proceed past validation to `--dry-run`'s
        # own PASS path instead of refusing here.
        case = harness.load_case("spec-draft-quality")
        run_model_mock = mock.Mock()
        with mock.patch.object(harness, "run_model", run_model_mock):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--model", case["grader_model"], "--dry-run"]
            )
        self.assertEqual(code, harness.EXIT_CONFIG, f"stdout:\n{out}\nstderr:\n{err}")
        self.assertIn(case["grader_model"], out + err)
        run_model_mock.assert_not_called()

    def test_main_accepts_a_distinct_model_flag(self) -> None:
        code, out, err = run_harness_main(
            ["--case", "spec-draft-quality", "--model", "claude-haiku-4-5", "--dry-run"]
        )
        self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")


class LiveDrafterModelRequiredTests(unittest.TestCase):
    """Reviewer fix A: a live draft-quality run must REQUIRE an explicit
    drafter --model, so the drafter/grader split is VALIDATED rather than
    assumed. The host default is not guaranteed to differ from
    grader_model, so omitting --model on a live run must refuse before any
    provider call. --dry-run may still omit --model.
    """

    def test_assert_live_drafter_model_required_refuses_unspecified(self) -> None:
        case = harness.load_case("spec-draft-quality")
        with self.assertRaises(harness.ConfigError) as ctx:
            harness.assert_live_drafter_model_required(case, None)
        self.assertIn(case["id"], str(ctx.exception))

    def test_assert_live_drafter_model_required_accepts_explicit_model(self) -> None:
        case = harness.load_case("spec-draft-quality")
        harness.assert_live_drafter_model_required(case, "claude-sonnet-5")  # must not raise

    def test_non_draft_kinds_have_no_drafter_model_requirement(self) -> None:
        case = harness.load_case("coder-brief-completeness")
        harness.assert_live_drafter_model_required(case, None)  # no-op, must not raise

    def test_main_live_without_model_flag_is_refused_before_any_provider_call(self) -> None:
        # Call-site mutant-kill evidence: deleting or no-opping the
        # `assert_live_drafter_model_required(case, args.model)` call in
        # `main()` would let a live run with no --model fall through to
        # `shutil.which` / the trial loop and reach `run_model`, which this
        # test observes directly.
        run_model_mock = mock.Mock()
        make_arena_mock = mock.Mock(side_effect=arena.make_arena)
        with mock.patch.object(harness, "run_model", run_model_mock), \
                mock.patch.object(harness.arena, "make_arena", make_arena_mock):
            code, out, err = run_harness_main(["--case", "spec-draft-quality"])
        self.assertEqual(code, harness.EXIT_CONFIG, f"stdout:\n{out}\nstderr:\n{err}")
        self.assertIn("--model", out + err)
        run_model_mock.assert_not_called()
        make_arena_mock.assert_not_called()

    def test_main_dry_run_without_model_flag_still_passes(self) -> None:
        # --dry-run never spawns a host, so it must still tolerate an
        # unspecified drafter model.
        code, out, err = run_harness_main(["--case", "spec-draft-quality", "--dry-run"])
        self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")

    def test_main_live_with_explicit_model_reaches_past_the_guard(self) -> None:
        # The positive half: an explicit --model must NOT be refused by this
        # guard (it may still be refused later, e.g. by auth).
        run_model_mock = mock.Mock()
        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness, "run_model", run_model_mock):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--model", "claude-haiku-4-5", "--trials", "1"]
            )
        self.assertNotIn("requires an explicit --model", out + err)


class BuildDraftPromptTests(unittest.TestCase):
    """Reviewer fix C: an embedded `"` in the goal text must not corrupt the
    `/pl-spec-draft "<goal>"` prompt's own quoted argument.
    """

    def test_plain_goal_is_wrapped_unescaped(self) -> None:
        prompt = harness.build_draft_prompt("add real-time notifications")
        self.assertEqual(prompt, '/pl-spec-draft "add real-time notifications"')

    def test_embedded_quotes_are_backslash_escaped(self) -> None:
        goal = 'support the "premium" tier'
        prompt = harness.build_draft_prompt(goal)
        self.assertEqual(prompt, '/pl-spec-draft "support the \\"premium\\" tier"')
        self.assertIn('\\"premium\\"', prompt)

    def test_embedded_backslash_before_quote_does_not_unescape_it(self) -> None:
        # Escaping the backslash FIRST is what stops a source `\"` from
        # colliding with an escape this function inserts.
        goal = 'a literal backslash-quote: \\" here'
        prompt = harness.build_draft_prompt(goal)
        self.assertEqual(prompt, '/pl-spec-draft "a literal backslash-quote: \\\\\\" here"')

    def test_multiline_goal_with_quotes_round_trips(self) -> None:
        goal = 'Line one says "go".\nLine two has no quotes.\nLine three says "stop" too.'
        prompt = harness.build_draft_prompt(goal)
        self.assertTrue(prompt.startswith('/pl-spec-draft "'))
        self.assertIn("Line two has no quotes.", prompt)
        self.assertIn('Line one says \\"go\\".', prompt)
        self.assertIn('Line three says \\"stop\\" too.', prompt)

    def test_goal_without_quotes_is_unaffected_by_escaping(self) -> None:
        goal = harness.read_asset(harness.load_case("spec-draft-quality"), "goal_fixture")
        self.assertNotIn('"', goal)
        prompt = harness.build_draft_prompt(goal)
        self.assertIn(goal, prompt)


class DrafterRoutingTests(unittest.TestCase):
    """Task hh-planning-grader-split: the live drafter runs through the
    installed `pl-spec-draft` surface, staged and authenticated before it
    spawns, and the grader call uses `grader_model`, never the drafter's.
    """

    _COMPLETE_DRAFT = (
        "product-spec tech-spec test-spec roadmap\n"
        "Goal: x\nNon-goals: y\nAcceptance: z\nOpen questions: w\n"
    )

    def test_trial_loop_stages_vendor_config_before_auth_and_host_spawn(self) -> None:
        # Call-site mutant-kill evidence: removing the
        # `arena.stage_vendor_config(trial_env, "claude")` call from the
        # trial loop would drop "stage:claude" from `call_order` entirely
        # and leave `stage_mock` uncalled.
        call_order: list[str] = []

        def _stage_side_effect(env, vendor, **kw):
            call_order.append(f"stage:{vendor}")

        def _auth_side_effect(env, vendor):
            call_order.append(f"auth:{vendor}")

        run_model_calls = []

        def _run_model_side_effect(argv, prompt, timeout, env):
            call_order.append("run_model")
            run_model_calls.append(1)
            return self._COMPLETE_DRAFT if len(run_model_calls) == 1 else '{"scores": {"a": 1.0}}'

        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "stage_vendor_config", side_effect=_stage_side_effect) as stage_mock, \
                mock.patch.object(harness.arena, "assert_vendor_auth", side_effect=_auth_side_effect), \
                mock.patch.object(harness, "run_model", side_effect=_run_model_side_effect), \
                mock.patch.object(harness, "persist_to_ledger"):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")
        stage_mock.assert_called_once()
        self.assertEqual(stage_mock.call_args.args[1], "claude")
        self.assertEqual(call_order[:2], ["stage:claude", "auth:claude"])

    def test_drafter_prompt_invokes_the_installed_pl_spec_draft_surface(self) -> None:
        prompts: list[tuple[list[str], str]] = []

        def _run_model_side_effect(argv, prompt, timeout, env):
            prompts.append((list(argv), prompt))
            if len(prompts) == 1:
                return self._COMPLETE_DRAFT
            return '{"scores": {"a": 1.0}}'

        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "stage_vendor_config"), \
                mock.patch.object(harness.arena, "assert_vendor_auth"), \
                mock.patch.object(harness, "run_model", side_effect=_run_model_side_effect), \
                mock.patch.object(harness, "persist_to_ledger"):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")
        self.assertEqual(len(prompts), 2)
        drafter_argv, drafter_prompt = prompts[0]
        self.assertTrue(
            drafter_prompt.startswith('/pl-spec-draft "'),
            f"drafter prompt did not invoke pl-spec-draft: {drafter_prompt[:80]!r}",
        )
        goal = harness.read_asset(harness.load_case("spec-draft-quality"), "goal_fixture")
        # The FULL multi-line goal, not just its first line: the shipped
        # fixture is several lines of prose, and a mutant that truncates
        # `build_draft_prompt` at the first newline would still pass a
        # first-line-only assertion while silently dropping the rest of the
        # goal the drafter is supposed to receive.
        self.assertGreater(goal.count("\n"), 1, "fixture must be multi-line for this assertion to mean anything")
        self.assertIn(goal, drafter_prompt)
        self.assertIn("--model", drafter_argv)
        self.assertEqual(drafter_argv[drafter_argv.index("--model") + 1], "claude-haiku-4-5")

    def test_grader_call_uses_the_case_grader_model_not_the_drafter_model(self) -> None:
        # Call-site mutant-kill evidence: if the grader's `run_model` call
        # used `args.model` (the drafter) instead of `case["grader_model"]`,
        # the second captured argv below would carry "claude-haiku-4-5"
        # instead of the case's grader_model, and the final assertion
        # would fail.
        case = harness.load_case("spec-draft-quality")
        argvs: list[list[str]] = []

        def _run_model_side_effect(argv, prompt, timeout, env):
            argvs.append(list(argv))
            return self._COMPLETE_DRAFT if len(argvs) == 1 else '{"scores": {"a": 1.0}}'

        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "stage_vendor_config"), \
                mock.patch.object(harness.arena, "assert_vendor_auth"), \
                mock.patch.object(harness, "run_model", side_effect=_run_model_side_effect), \
                mock.patch.object(harness, "persist_to_ledger"):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")
        drafter_argv, grader_argv = argvs
        self.assertIn("--model", drafter_argv)
        self.assertEqual(drafter_argv[drafter_argv.index("--model") + 1], "claude-haiku-4-5")
        self.assertIn("--model", grader_argv)
        self.assertEqual(grader_argv[grader_argv.index("--model") + 1], case["grader_model"])
        self.assertNotEqual(case["grader_model"], "claude-haiku-4-5")

    def test_result_records_both_models(self) -> None:
        import json as _json

        case = harness.load_case("spec-draft-quality")

        def _run_model_side_effect(argv, prompt, timeout, env):
            calls = _run_model_side_effect.calls
            calls.append(1)
            return self._COMPLETE_DRAFT if len(calls) == 1 else '{"scores": {"a": 1.0}}'

        _run_model_side_effect.calls = []

        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "stage_vendor_config"), \
                mock.patch.object(harness.arena, "assert_vendor_auth"), \
                mock.patch.object(harness, "run_model", side_effect=_run_model_side_effect), \
                mock.patch.object(harness, "persist_to_ledger"):
            code, out, _err = run_harness_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_PASS, out)
        recorded = None
        for line in out.splitlines():
            try:
                obj = _json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict) and obj.get("case") == case["id"]:
                recorded = obj
                break
        self.assertIsNotNone(recorded, f"no result JSON line found in:\n{out}")
        self.assertEqual(recorded["drafter_model"], "claude-haiku-4-5")
        self.assertEqual(recorded["grader_model"], case["grader_model"])

    def test_vendor_staging_failure_fails_closed_before_host_spawn(self) -> None:
        run_model_mock = mock.Mock()
        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(
                    harness.arena, "stage_vendor_config",
                    side_effect=arena.VendorStagingError("claude: required read surface is missing: /x/commands"),
                ), \
                mock.patch.object(harness, "run_model", run_model_mock):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_FAIL)
        self.assertIn("required read surface", out + err)
        run_model_mock.assert_not_called()


class LedgerPersistenceTests(unittest.TestCase):
    """Task 6894: `{drafter_model, grader_model}` land in the durable,
    shared `evals/RESULTS.md` ledger via the orchestrator harness's
    `record_ledger_row` (task 6860 / C6), not only in the printed JSON
    line -- a live planning run is paid-for, and losing which model
    drafted and which graded makes the retained result unattributable.
    """

    _COMPLETE_DRAFT = (
        "product-spec tech-spec test-spec roadmap\n"
        "Goal: x\nNon-goals: y\nAcceptance: z\nOpen questions: w\n"
    )

    @staticmethod
    def _draft_then_grade_side_effect(calls: list[int]):
        def _side_effect(argv, prompt, timeout, env):
            calls.append(1)
            return (
                LedgerPersistenceTests._COMPLETE_DRAFT
                if len(calls) == 1
                else '{"scores": {"a": 1.0}}'
            )

        return _side_effect

    def test_persist_to_ledger_writes_case_and_both_models(self) -> None:
        orch = harness._orchestrator_harness()
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            with mock.patch.object(orch, "RESULTS_LEDGER_PATH", ledger_path):
                harness.persist_to_ledger(
                    {"id": "persist-probe"},
                    "claude-haiku-4-5",
                    "claude-opus-5",
                    {"mean": 0.9, "min": 0.8, "max": 1.0, "stdev": 0.1},
                    "pass",
                )
                rows = orch.ledger_rows()
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["case"], "persist-probe")
            self.assertEqual(rows[0]["grade"], "pass")
            self.assertIn("claude-haiku-4-5", rows[0]["model"])
            self.assertIn("claude-opus-5", rows[0]["model"])

    def test_call_site_records_a_ledger_row_for_a_live_run(self) -> None:
        # CALL-SITE test: proves `main()` itself invokes `persist_to_ledger`,
        # not just that the function works standing alone. A no-op in place
        # of the call site inside `main()` would leave the test above green
        # while a real `--case ... --model ...` invocation never populated
        # the ledger (task 6917/6894 review finding: unit-test isolation
        # from the previous cycle is exactly why this needs its own
        # end-to-end proof, not just a call-site-unaware unit check).
        orch = harness._orchestrator_harness()
        calls: list[int] = []
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                    mock.patch.object(harness.arena, "stage_vendor_config"), \
                    mock.patch.object(harness.arena, "assert_vendor_auth"), \
                    mock.patch.object(
                        harness, "run_model",
                        side_effect=self._draft_then_grade_side_effect(calls),
                    ), \
                    mock.patch.object(orch, "RESULTS_LEDGER_PATH", ledger_path):
                code, out, err = run_harness_main(
                    ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
                )
                rows = orch.ledger_rows()
            self.assertEqual(code, harness.EXIT_PASS, f"stdout:\n{out}\nstderr:\n{err}")
            case = harness.load_case("spec-draft-quality")
            self.assertEqual(len(rows), 1, f"expected exactly one ledger row, got: {rows}")
            self.assertEqual(rows[0]["case"], case["id"])
            self.assertEqual(rows[0]["grade"], "pass")
            self.assertIn("claude-haiku-4-5", rows[0]["model"])
            self.assertIn(case["grader_model"], rows[0]["model"])

    def test_ledger_persist_failure_fails_the_run_not_silently(self) -> None:
        # "Failure over silence" (this file's own docstring doctrine):
        # a persistence failure must fail the run, never a pass and never
        # a silent skip.
        calls: list[int] = []
        with mock.patch.object(harness.shutil, "which", return_value="/usr/bin/claude"), \
                mock.patch.object(harness.arena, "stage_vendor_config"), \
                mock.patch.object(harness.arena, "assert_vendor_auth"), \
                mock.patch.object(
                    harness, "run_model",
                    side_effect=self._draft_then_grade_side_effect(calls),
                ), \
                mock.patch.object(
                    harness, "persist_to_ledger", side_effect=RuntimeError("disk full")
                ):
            code, out, err = run_harness_main(
                ["--case", "spec-draft-quality", "--trials", "1", "--model", "claude-haiku-4-5"]
            )
        self.assertEqual(code, harness.EXIT_FAIL)
        self.assertIn("disk full", out + err)


if __name__ == "__main__":
    unittest.main(verbosity=1)
