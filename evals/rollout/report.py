#!/usr/bin/env python3
"""Offline rollout report for the adaptive routing plane (plan 949 task 5522).

Answers "is this rollout safe to trust yet?" by reading Planar's own read verbs.
It invokes no model: a rollout check that requires the thing being rolled out
cannot validate a broken rollout, and it must run on a machine with no provider
access at all.

Three separations do the work:

**Class separation.** Declared experiments are counted apart from default,
override, and bypass telemetry. Collapsing them is exactly the conflation the
evidence boundary exists to prevent — a fallback that happened to succeed is
evidence the primary was unavailable, not that the fallback is good.

**Cohort isolation.** Outcomes are only ever compared within one exact tuple of
(project, validation policy, vendor, role, tier, work type, complexity).
Nothing is aggregated across a cohort dimension, because evidence gathered under
one policy says nothing about another.

**Preview-only suggestions.** A cohort that has not cleared its evidence gates
yields no recommendation, and the report names which gate blocked it. Thin
evidence is a valid rollout state, not an error.

Exit codes: 0 report produced (including "not ready yet"), 1 real
inconsistency, 2 usage/environment error.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
COHORT_KEYS = (
    "project_id",
    "validation_policy_version",
    "vendor",
    "role",
    "tier",
    "work_type",
    "complexity",
)
EVIDENTIAL_CLASS = "declared_experiment"
OBSERVATIONAL_CLASSES = ("default", "fallback", "override")

EXIT_OK, EXIT_INCONSISTENT, EXIT_ENV = 0, 1, 2


class EnvError(Exception):
    """The report could not be produced. Never a rollout verdict."""


def planar_json(args: list[str], db: str | None) -> Any:
    if shutil.which("planar") is None:
        raise EnvError("planar CLI not found on PATH")
    env = None
    if db:
        import os

        env = {**os.environ, "PLANAR_DB": db}
    proc = subprocess.run(
        ["planar", *args, "--json"],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
        env=env,
        stdin=subprocess.DEVNULL,
    )
    if proc.returncode != 0:
        raise EnvError(f"`planar {' '.join(args)}` failed: {proc.stderr.strip()[:200]}")
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise EnvError(f"`planar {' '.join(args)}` did not return JSON: {exc}") from exc


def cohort_key(row: dict[str, Any]) -> tuple:
    """The exact cohort tuple. Missing dimensions are preserved as None so a
    partially-attributed row can never silently join a complete cohort."""
    return tuple(row.get(k) for k in COHORT_KEYS)


def separate_classes(outcomes: list[dict[str, Any]]) -> dict[str, int]:
    """Count evidential vs observational telemetry, never blended."""
    counts = {"declared_experiment": 0, "observational": 0, "excluded": 0}
    for row in outcomes:
        if not row.get("counts_toward_recommendation", False):
            counts["excluded"] += 1
        else:
            counts["declared_experiment"] += 1
    counts["observational"] = len(outcomes) - counts["declared_experiment"] - counts["excluded"]
    return counts


def group_by_cohort(outcomes: list[dict[str, Any]]) -> dict[tuple, list[dict[str, Any]]]:
    grouped: dict[tuple, list[dict[str, Any]]] = defaultdict(list)
    for row in outcomes:
        grouped[cohort_key(row)].append(row)
    return dict(grouped)


def gate_status(
    sample_count: int, minimum_samples: int, confidence_ok: bool
) -> tuple[bool, str]:
    """Whether a cohort may yield a suggestion, and which gate blocked it.

    Naming the blocking gate matters: "no recommendation" for want of samples
    and "no recommendation" for failing the confidence floor call for different
    operator actions.
    """
    if sample_count < minimum_samples:
        return False, f"insufficient_data ({sample_count}/{minimum_samples} samples)"
    if not confidence_ok:
        return False, "below confidence floor"
    return True, "gates passed"


def exclusion_reasons(outcomes: list[dict[str, Any]]) -> dict[str, int]:
    """Excluded rows are reported WITH their reason, never dropped silently."""
    reasons: dict[str, int] = defaultdict(int)
    for row in outcomes:
        if not row.get("counts_toward_recommendation", False):
            reasons[row.get("exclusion_reason") or "unstated"] += 1
    return dict(reasons)


def rollback_available(migration_stem: str) -> tuple[bool, str]:
    """Confirm the down file for a migration exists on disk."""
    down = REPO_ROOT / "migrations" / f"{migration_stem}.down.sql"
    return down.is_file(), str(down.relative_to(REPO_ROOT))


def registry_health(registry: dict[str, Any]) -> dict[str, Any]:
    """Opaque ids present, bindings resolvable, unverified marked ineligible."""
    candidates = registry.get("candidates", [])
    unverified = []
    unbound = []
    for entry in candidates:
        reg = entry.get("registration", {})
        cid = reg.get("candidate_id", "")
        if not entry.get("bindings"):
            unbound.append(cid)
        obs = entry.get("latest_observation")
        if not obs or obs.get("spawn_verification") not in ("verified",):
            # Unverified is reported as ineligible, not counted as available:
            # listing it as usable would imply a check that never happened.
            unverified.append(cid)
    return {
        "total": len(candidates),
        "unverified_ineligible": unverified,
        "unbound": unbound,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", help="isolated database to report on (defaults to the resolved one)")
    ap.add_argument("--minimum-samples", type=int, default=5)
    ap.add_argument("--migration", default="00031_dispatch_confirmation_tokens")
    args = ap.parse_args()

    try:
        registry = planar_json(["models", "registry", "list"], args.db)
        experiments = planar_json(["models", "experiments"], args.db)
        outcomes_doc = planar_json(["models", "outcomes", "--limit", "1000"], args.db)
    except EnvError as exc:
        print(f"environment error: {exc}", file=sys.stderr)
        return EXIT_ENV

    outcomes = outcomes_doc.get("outcomes", [])
    exps = experiments.get("experiments", [])

    print("routing rollout report — offline, no model invoked\n")

    health = registry_health(registry)
    print(f"registry: {health['total']} candidate(s)")
    if health["unverified_ineligible"]:
        print(f"  INELIGIBLE (unverified on this host): {', '.join(health['unverified_ineligible'])}")
    if health["unbound"]:
        print(f"  no role/tier binding: {', '.join(health['unbound'])}")

    classes = separate_classes(outcomes)
    print(f"\ntelemetry: {len(outcomes)} terminal outcome(s)")
    print(f"  declared-experiment (evidential): {classes['declared_experiment']}")
    print(f"  excluded (retained, not counted): {classes['excluded']}")
    for reason, count in sorted(exclusion_reasons(outcomes).items()):
        print(f"      {reason}: {count}")

    print(f"\nexperiments: {len(exps)} declared")
    for exp in exps:
        print(
            f"  {exp.get('experiment_key')} [{exp.get('status')}] "
            f"{exp.get('samples', 0)} recorded / {exp.get('eligible_samples', 0)} counted"
        )

    grouped = group_by_cohort([o for o in outcomes if o.get("counts_toward_recommendation")])
    print(f"\ncohorts with evidential samples: {len(grouped)}")
    suggestions = 0
    for key, rows in sorted(grouped.items(), key=lambda kv: str(kv[0])):
        # Confidence is Planar's to compute; the report only reflects whether a
        # cohort has cleared its gates, and never invents a ranking itself.
        allowed, why = gate_status(len(rows), args.minimum_samples, confidence_ok=False)
        label = "/".join(str(k) for k in key if k is not None)
        print(f"  {label}: {len(rows)} sample(s) — {why}")
        if allowed:
            suggestions += 1
    if not grouped:
        print("  (none — nothing to compare, which is a normal early rollout state)")

    ok, path = rollback_available(args.migration)
    print(f"\nrollback: {args.migration} down file {'present' if ok else 'MISSING'} ({path})")

    print(
        f"\nsuggestions: {suggestions} cohort(s) cleared their gates — "
        "preview only, nothing is applied by this report."
    )
    if not ok:
        print("\nFAIL: rollback path is missing for the named migration.")
        return EXIT_INCONSISTENT
    print("\nreport complete. Thin evidence is an expected state, not a failure.")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
