#!/usr/bin/env python3
"""Offline tests for the rollout report's pure logic."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import report  # noqa: E402


def outcome(counts: bool, reason: str | None = None, **cohort: object) -> dict:
    base = {
        "project_id": 1,
        "validation_policy_version": "val-v1",
        "vendor": "vendor-x",
        "role": "coder",
        "tier": "medium",
        "work_type": "feature",
        "complexity": "standard",
        "counts_toward_recommendation": counts,
        "exclusion_reason": reason,
    }
    base.update(cohort)
    return base


class ClassSeparationTests(unittest.TestCase):
    def test_evidential_and_excluded_are_counted_apart(self) -> None:
        # Blending them is the conflation the evidence boundary exists to stop.
        rows = [outcome(True), outcome(True), outcome(False, "fallback_assignment")]
        counts = report.separate_classes(rows)
        self.assertEqual(counts["declared_experiment"], 2)
        self.assertEqual(counts["excluded"], 1)

    def test_exclusion_reasons_are_reported_not_dropped(self) -> None:
        rows = [outcome(False, "candidate_mismatch"), outcome(False, "review_bypassed")]
        reasons = report.exclusion_reasons(rows)
        self.assertEqual(reasons["candidate_mismatch"], 1)
        self.assertEqual(reasons["review_bypassed"], 1)

    def test_an_excluded_row_without_a_reason_is_still_surfaced(self) -> None:
        # Silence here would hide a row that failed to record why it was cut.
        self.assertEqual(report.exclusion_reasons([outcome(False, None)]), {"unstated": 1})


class CohortIsolationTests(unittest.TestCase):
    def test_differing_on_any_dimension_makes_a_separate_cohort(self) -> None:
        base = outcome(True)
        for dimension, other in [
            ("project_id", 2),
            ("validation_policy_version", "val-v2"),
            ("vendor", "vendor-y"),
            ("role", "reviewer"),
            ("tier", "large"),
            ("work_type", "architectural"),
            ("complexity", "high-risk"),
        ]:
            grouped = report.group_by_cohort([base, outcome(True, **{dimension: other})])
            self.assertEqual(len(grouped), 2, f"{dimension} must not be pooled")

    def test_identical_cohorts_group_together(self) -> None:
        self.assertEqual(len(report.group_by_cohort([outcome(True), outcome(True)])), 1)

    def test_a_partially_attributed_row_cannot_join_a_complete_cohort(self) -> None:
        # A missing dimension is preserved as None rather than defaulted, so an
        # unattributed run cannot inherit another cohort's evidence.
        partial = outcome(True)
        del partial["work_type"]
        self.assertEqual(len(report.group_by_cohort([outcome(True), partial])), 2)


class GateTests(unittest.TestCase):
    def test_thin_evidence_blocks_a_suggestion_and_names_the_gate(self) -> None:
        allowed, why = report.gate_status(2, 5, confidence_ok=True)
        self.assertFalse(allowed)
        self.assertIn("insufficient_data", why)
        self.assertIn("2/5", why)

    def test_confidence_floor_is_a_distinct_blocker(self) -> None:
        # "Not enough samples" and "samples disagree" need different operator
        # actions, so they must not report the same reason.
        allowed, why = report.gate_status(50, 5, confidence_ok=False)
        self.assertFalse(allowed)
        self.assertIn("confidence", why)

    def test_gates_pass_only_when_both_hold(self) -> None:
        allowed, _ = report.gate_status(50, 5, confidence_ok=True)
        self.assertTrue(allowed)


class RegistryHealthTests(unittest.TestCase):
    def test_unverified_candidate_is_ineligible_not_available(self) -> None:
        registry = {
            "candidates": [
                {
                    "registration": {"candidate_id": "cand-a"},
                    "bindings": [{"role": "coder"}],
                    "latest_observation": {"spawn_verification": "unverified"},
                }
            ]
        }
        health = report.registry_health(registry)
        self.assertIn("cand-a", health["unverified_ineligible"])

    def test_verified_and_bound_candidate_is_clean(self) -> None:
        registry = {
            "candidates": [
                {
                    "registration": {"candidate_id": "cand-a"},
                    "bindings": [{"role": "coder"}],
                    "latest_observation": {"spawn_verification": "verified"},
                }
            ]
        }
        health = report.registry_health(registry)
        self.assertEqual(health["unverified_ineligible"], [])
        self.assertEqual(health["unbound"], [])

    def test_missing_observation_counts_as_unverified(self) -> None:
        registry = {"candidates": [{"registration": {"candidate_id": "c"}, "bindings": [{}]}]}
        self.assertIn("c", report.registry_health(registry)["unverified_ineligible"])


class RollbackTests(unittest.TestCase):
    def test_shipped_migration_has_a_down_file(self) -> None:
        ok, _ = report.rollback_available("00031_dispatch_confirmation_tokens")
        self.assertTrue(ok)

    def test_absent_migration_is_reported_missing(self) -> None:
        ok, _ = report.rollback_available("99999_not_a_migration")
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main(verbosity=1)
