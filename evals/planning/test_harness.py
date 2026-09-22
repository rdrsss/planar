#!/usr/bin/env python3
"""Offline tests for the deterministic paths of the planning eval harness."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Appended, not inserted at 0: both directories have a `harness.py`, and
# `import harness` here must resolve to THIS harness, not the orchestrator's.
sys.path.append(str(Path(__file__).resolve().parents[1] / "orchestrator"))

import harness  # noqa: E402
import arena  # noqa: E402

HARNESS_PATH = Path(__file__).resolve().parent / "harness.py"


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


if __name__ == "__main__":
    unittest.main(verbosity=1)
