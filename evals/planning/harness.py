#!/usr/bin/env python3
"""Semantic evaluation lines for the planning surfaces (plan 948).

These grade *judgement*, not contract text, which forces three properties the
contract suites do not need:

**Trials, not a single sample.** One semantic run cannot distinguish a capable
model from a lucky one. Cases declare a trial count and results report spread —
the same reason routing ranks on a confidence bound rather than a raw rate.

**An independent, versioned grader.** Grading is a separate invocation whose
prompt carries an explicit version, so changing the grader shows up in results
instead of silently reinterpreting old ones. The grader is never told which
model produced the draft; attribution would let reputation stand in for
evidence.

**Failure over silence.** A grader that errors, returns unparseable output, or
is blocked FAILS the run. It is never a pass and never a silent skip — that
failure mode (a check that looks green while measuring nothing) has bitten this
repo repeatedly.

Structural checks run first and are deterministic, so a malformed draft fails
before any semantic grading is attempted or paid for.

Exit codes: 0 pass, 1 fail, 2 usage/config error, 75 blocked (credentials or
rate limit — neither a pass nor a failure).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping

EVAL_ROOT = Path(__file__).resolve().parent
REPO_ROOT = EVAL_ROOT.parents[1]

# `arena.py` lives under evals/orchestrator/ (task hh-eval-arena); it is the
# shared isolation module both harnesses use, so the planning harness need
# not keep its own copy of the arena env / isolation-assertion logic.
# Appended, not inserted at 0: both directories have a `harness.py`, and a
# caller that runs this file as a script must not have `import harness`
# elsewhere resolve to the orchestrator's module instead of this one.
sys.path.append(str(EVAL_ROOT.parent / "orchestrator"))
import arena  # noqa: E402

EXIT_PASS, EXIT_FAIL, EXIT_CONFIG, EXIT_BLOCKED = 0, 1, 2, 75

BLOCKED = re.compile(
    r"rate limit|resource_exhausted|429|quota|overloaded|usage limit"
    r"|not authenticated|no api key|credential",
    re.IGNORECASE,
)


class ConfigError(Exception):
    """The case or its assets are unusable. Never reported as a test failure."""


class GraderError(Exception):
    """The grader could not produce a verdict. ALWAYS a run failure."""


class Blocked(Exception):
    """Provider unavailable. Neither pass nor fail."""


@dataclass
class StructuralResult:
    ok: bool
    missing_artifacts: list[str] = field(default_factory=list)
    missing_sections: list[str] = field(default_factory=list)
    forbidden_found: list[str] = field(default_factory=list)

    def reasons(self) -> list[str]:
        out = []
        if self.missing_artifacts:
            out.append(f"missing artifacts: {', '.join(self.missing_artifacts)}")
        if self.missing_sections:
            out.append(f"missing sections: {', '.join(self.missing_sections)}")
        if self.forbidden_found:
            # A draft claiming work it did not do is the strict-preview
            # failure: it reads as progress and is worse than an obvious gap.
            out.append(f"unsupported claims: {', '.join(self.forbidden_found)}")
        return out


@dataclass
class TrialResult:
    index: int
    score: float
    scores: dict[str, float]
    structural: StructuralResult


def load_case(case_id: str) -> dict[str, Any]:
    path = EVAL_ROOT / "cases" / f"{case_id}.json"
    if not path.is_file():
        available = sorted(p.stem for p in (EVAL_ROOT / "cases").glob("*.json"))
        raise ConfigError(
            f"unknown CASE '{case_id}'. Available: {', '.join(available) or '(none)'}"
        )
    case = json.loads(path.read_text(encoding="utf-8"))
    kind = case.get("kind", "draft-quality")
    # Each kind names its own required keys, so a case missing an asset fails
    # at load with the key named rather than at run time with a KeyError.
    required_by_kind = {
        "draft-quality": ("id", "trials", "goal_fixture", "grader", "grader_version", "structural"),
        "seeded-recall": (
            "id", "trials", "spec_fixture", "manifest_fixture", "grader",
            "grader_version", "severity_weights",
        ),
        "sentinel-lineage": ("id", "trials", "entities", "redaction"),
        "packet-readiness": ("id", "trials", "required_fields", "generic_markers"),
        "brief-completeness": ("id", "trials", "mandatory_elements"),
    }
    if kind not in required_by_kind:
        raise ConfigError(
            f"case '{case_id}' has unknown kind '{kind}'. "
            f"Known: {', '.join(sorted(required_by_kind))}"
        )
    for required in required_by_kind[kind]:
        if required not in case:
            raise ConfigError(f"case '{case_id}' ({kind}) is missing required key '{required}'")
    if int(case["trials"]) < 1:
        raise ConfigError(f"case '{case_id}' must declare at least one trial")
    case["kind"] = kind
    return case


def load_manifest(case: dict[str, Any]) -> list[dict[str, Any]]:
    raw = read_asset(case, "manifest_fixture")
    try:
        obj = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ConfigError(f"defect manifest is not valid JSON: {exc}") from exc
    defects = obj.get("defects")
    if not isinstance(defects, list) or not defects:
        raise ConfigError("defect manifest has no 'defects' list")
    for defect in defects:
        for key in ("id", "severity", "summary"):
            if key not in defect:
                raise ConfigError(f"manifest defect missing '{key}': {defect}")
    return defects


def read_asset(case: dict[str, Any], key: str) -> str:
    path = EVAL_ROOT / case[key]
    if not path.is_file():
        raise ConfigError(f"case '{case['id']}' references missing {key}: {path}")
    return path.read_text(encoding="utf-8")


def check_structure(draft: str, spec: dict[str, Any]) -> StructuralResult:
    """Deterministic gate. No model, no cost, runs before semantic grading."""
    lowered = draft.lower()
    missing_artifacts = [a for a in spec.get("required_artifacts", []) if a.lower() not in lowered]
    missing_sections = [s for s in spec.get("required_sections", []) if s.lower() not in lowered]
    forbidden = [f for f in spec.get("forbidden_claims", []) if f.lower() in lowered]
    return StructuralResult(
        ok=not (missing_artifacts or missing_sections or forbidden),
        missing_artifacts=missing_artifacts,
        missing_sections=missing_sections,
        forbidden_found=forbidden,
    )


def parse_grader_output(raw: str) -> dict[str, float]:
    """Extract the scores object, or raise GraderError.

    Unparseable output is a FAILURE, not a zero: a zero would be a verdict, and
    we do not have one. Conflating "graded badly" with "did not grade" is how a
    broken grader starts looking like a consistently bad model.
    """
    match = re.search(r"\{.*\}", raw, re.DOTALL)
    if not match:
        raise GraderError("grader returned no JSON object")
    try:
        obj = json.loads(match.group(0))
    except json.JSONDecodeError as exc:
        raise GraderError(f"grader JSON did not parse: {exc}") from exc
    scores = obj.get("scores")
    if not isinstance(scores, dict) or not scores:
        raise GraderError("grader output has no 'scores' object")
    out: dict[str, float] = {}
    for key, value in scores.items():
        if not isinstance(value, (int, float)):
            raise GraderError(f"grader score '{key}' is not numeric: {value!r}")
        if not 0.0 <= float(value) <= 1.0:
            raise GraderError(f"grader score '{key}' out of range: {value}")
        out[key] = float(value)
    return out


@dataclass
class BriefVerdict:
    ok: bool
    missing: list[str] = field(default_factory=list)
    altered: list[str] = field(default_factory=list)
    unsafe: list[str] = field(default_factory=list)

    def reasons(self) -> list[str]:
        out = []
        if self.missing:
            out.append(f"missing: {', '.join(self.missing)}")
        if self.altered:
            out.append(f"altered: {', '.join(self.altered)}")
        if self.unsafe:
            out.append(f"unsafe values: {', '.join(self.unsafe)}")
        return out


def safe_brief_value(value: str) -> bool:
    """Punctuation is content; control characters are not.

    A coder brief carries operator prose, so `;`, `$()` and backticks are
    ordinary text and rejecting them would break real briefs. Control
    characters are refused because they truncate or corrupt the record — the
    same rule the routing surfaces apply to opaque model ids.
    """
    return not any(ord(c) < 0x20 and c not in "\n\t" or ord(c) == 0x7F for c in value)


def check_brief_completeness(
    brief: dict[str, Any],
    source: dict[str, Any],
    mandatory: list[str],
) -> BriefVerdict:
    """Compare a brief against the packet it was built from.

    Every problem is reported, not just the first, so one pass tells the
    operator everything to fix.

    An ALTERED element is the dangerous case and is treated as severely as a
    missing one: a brief with a silently rewritten acceptance criterion looks
    complete, so the coder proceeds confidently on context that is wrong.
    """
    verdict = BriefVerdict(ok=True)
    for element in mandatory:
        if element not in brief or brief[element] in (None, "", [], {}):
            verdict.missing.append(element)
            continue
        value = brief[element]
        if isinstance(value, str) and not safe_brief_value(value):
            verdict.unsafe.append(element)
            continue
        if element in source and source[element] != value:
            verdict.altered.append(element)
    verdict.ok = not (verdict.missing or verdict.altered or verdict.unsafe)
    return verdict


@dataclass
class ReadinessVerdict:
    ready: bool
    reasons: list[str] = field(default_factory=list)


def check_packet_readiness(
    packet: dict[str, Any],
    required_fields: list[str],
    generic_markers: list[str],
) -> ReadinessVerdict:
    """Decide whether a pulled packet is implementation-ready, and say why not.

    A bare boolean would be useless here: an operator needs to know WHICH field
    is missing or generic, because the remedy differs entirely.

    Generic content is rejected rather than noted. A packet whose acceptance
    criteria say "is implemented and tested" has no definition of done, so the
    run it produces cannot be judged — which is precisely the evidence the
    routing plane must never collect.
    """
    reasons: list[str] = []
    for name in required_fields:
        value = packet.get(name)
        if not isinstance(value, str) or not value.strip():
            reasons.append(f"missing required field: {name}")
            continue
        lowered = value.strip().lower()
        for marker in generic_markers:
            if lowered == marker.lower() or marker.lower() in lowered:
                reasons.append(f"generic {name}: matches placeholder '{marker}'")
                break
    return ReadinessVerdict(ready=not reasons, reasons=reasons)


def check_digest_freshness(confirmed_digest: str, current_digest: str) -> ReadinessVerdict:
    """Refuse a dispatch whose packet moved after confirmation.

    Accepting it would run against state the operator never saw — the same
    failure the preview/confirm contract exists to prevent, asserted here from
    the consumer side.
    """
    if not confirmed_digest or not current_digest:
        return ReadinessVerdict(False, ["digest missing on one side; cannot prove freshness"])
    if confirmed_digest != current_digest:
        return ReadinessVerdict(False, [f"stale digest: confirmed {confirmed_digest[:12]}… but current {current_digest[:12]}…"])
    return ReadinessVerdict(True)


def check_no_claim_leak(claims_after: list[dict[str, Any]], task_id: int) -> ReadinessVerdict:
    """After cancellation the task must hold no active claim.

    A stranded claim hides the work from the next pull until its lease expires,
    so the task looks like it is still being done by a process that is gone.
    """
    stranded = [
        c for c in claims_after
        if c.get("entity_kind") == "task"
        and c.get("entity_id") == task_id
        and c.get("status") == "active"
    ]
    if stranded:
        return ReadinessVerdict(False, [f"{len(stranded)} active claim(s) still held after cancellation"])
    return ReadinessVerdict(True)


@dataclass
class Sentinel:
    slot: str
    kind: str
    value: str


@dataclass
class LineageResult:
    ok: bool
    misplaced: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    missing_digest: list[str] = field(default_factory=list)
    duplicated: list[str] = field(default_factory=list)


def mint_sentinels(entities: list[dict[str, Any]], run_id: str) -> list[Sentinel]:
    """One unique sentinel per entity slot.

    Uniqueness per run is what stops a leftover row from a previous run being
    read as a fresh pass — the eval would then be green while ingestion was
    completely broken.
    """
    if not run_id or any(c.isspace() for c in run_id):
        raise ConfigError("run id must be a non-empty single token")
    out: list[Sentinel] = []
    for entity in entities:
        out.append(
            Sentinel(
                slot=entity["slot"],
                kind=entity["kind"],
                value=f"SENTINEL-{run_id}-{entity['slot'].upper()}",
            )
        )
    return out


def check_lineage(
    sentinels: list[Sentinel],
    observed: dict[str, list[str]],
    digests: dict[str, str] | None = None,
    require_digest: set[str] | None = None,
) -> LineageResult:
    """Verify each sentinel arrived in the CORRECT entity kind, exactly once.

    `observed` maps entity kind -> sentinel values found in that kind.

    Placement is the property, not presence. A sentinel seeded as a decision
    that surfaces as a question means ingestion mangled the lineage, and
    accepting "the text exists somewhere" would pass that broken run.
    """
    result = LineageResult(ok=True)
    require_digest = require_digest or set()
    digests = digests or {}
    everywhere: dict[str, list[str]] = {}
    for kind, values in observed.items():
        for value in values:
            everywhere.setdefault(value, []).append(kind)

    for sentinel in sentinels:
        landed = everywhere.get(sentinel.value, [])
        if not landed:
            result.missing.append(sentinel.slot)
            continue
        if len(landed) > 1 or landed.count(sentinel.kind) > 1:
            result.duplicated.append(sentinel.slot)
        if sentinel.kind not in landed:
            result.misplaced.append(f"{sentinel.slot}: expected {sentinel.kind}, found {landed[0]}")
            continue
        if sentinel.slot in require_digest and not digests.get(sentinel.slot):
            # Lineage proven at the first hop only is not lineage.
            result.missing_digest.append(sentinel.slot)

    result.ok = not (
        result.missing or result.misplaced or result.duplicated or result.missing_digest
    )
    return result


def redact_diagnostics(text: str, sentinels: list[Sentinel], source_bodies: list[str]) -> str:
    """Keep synthetic sentinels, remove real source bodies.

    A failing run must stay debuggable without persisting document contents:
    sentinels are values this eval invented, source bodies are not.
    """
    out = text
    for body in source_bodies:
        for line in body.splitlines():
            stripped = line.strip()
            if len(stripped) >= 12 and not any(s.value in stripped for s in sentinels):
                out = out.replace(stripped, "[redacted]")
    return out


def build_host_env(root: Path) -> dict[str, str]:
    """Build and isolation-check the subprocess environment for a live host call.

    Generalizes the former `assert_isolated_db(path, created_by_eval)`: a
    database path under an arena root that only `arena.make_arena()`
    created is, by construction, one this eval created — the same class of
    accident (a fixture path that silently resolved somewhere real, plan
    950) that guard existed to catch, now caught for every arena-owned
    variable rather than only `PLANAR_DB`. Raises `arena.ArenaIsolationError`
    before any host process starts if isolation does not hold.
    """
    env = arena.make_arena(root)
    arena.assert_isolated(env, root)
    return env


@dataclass
class RecallResult:
    recall: float
    precision: float
    matched: list[str]
    missed: list[str]
    unsupported: int
    total_findings: int


def severity_recall(
    manifest: list[dict[str, Any]],
    matched_ids: list[str],
    weights: dict[str, int],
) -> tuple[float, list[str]]:
    """Severity-weighted recall, plus the ids that were missed.

    Unweighted recall lets a reviewer pass by finding the easy majority while
    missing the one defect that matters. Weighting makes a critical miss cost
    more than several cosmetic ones, which is the actual review contract.
    """
    if not manifest:
        raise ConfigError("defect manifest is empty; nothing to recall")
    matched = set(matched_ids)
    total = 0.0
    found = 0.0
    missed: list[str] = []
    for defect in manifest:
        weight = float(weights.get(defect["severity"], 1))
        total += weight
        if defect["id"] in matched:
            found += weight
        else:
            missed.append(defect["id"])
    return (found / total if total else 0.0), missed


def precision(total_findings: int, unsupported: int) -> float:
    """Share of findings that correspond to a real seeded defect.

    This is what makes shotgunning fail. A review that lists everything reaches
    perfect recall, so recall alone cannot distinguish review from noise — and
    flagging everything is the cheapest way to game a recall metric.
    """
    if total_findings <= 0:
        # No findings at all is not perfect precision; it is no evidence of
        # precision. Returning 1.0 here would let an empty review pass this gate.
        return 0.0
    supported = max(0, total_findings - max(0, unsupported))
    return supported / total_findings


def score_seeded_recall(
    manifest: list[dict[str, Any]],
    grader: dict[str, Any],
    weights: dict[str, int],
) -> RecallResult:
    matched_ids = [m for m in grader.get("matched", []) if isinstance(m, str)]
    known = {d["id"] for d in manifest}
    unknown = [m for m in matched_ids if m not in known]
    if unknown:
        # The grader invented an id. Trusting it would silently inflate recall.
        raise GraderError(f"grader matched unknown defect ids: {', '.join(sorted(unknown))}")
    total_findings = int(grader.get("total_findings", 0))
    unsupported = int(grader.get("unsupported_count", 0))
    if unsupported > total_findings:
        raise GraderError(
            f"grader reported {unsupported} unsupported of {total_findings} findings"
        )
    rec, missed = severity_recall(manifest, matched_ids, weights)
    return RecallResult(
        recall=rec,
        precision=precision(total_findings, unsupported),
        matched=matched_ids,
        missed=missed,
        unsupported=unsupported,
        total_findings=total_findings,
    )


def parse_recall_grader(raw: str) -> dict[str, Any]:
    """Parse the matching grader's JSON, failing rather than guessing."""
    match = re.search(r"\{.*\}", raw, re.DOTALL)
    if not match:
        raise GraderError("grader returned no JSON object")
    try:
        obj = json.loads(match.group(0))
    except json.JSONDecodeError as exc:
        raise GraderError(f"grader JSON did not parse: {exc}") from exc
    for key in ("matched", "unsupported_count", "total_findings"):
        if key not in obj:
            raise GraderError(f"grader output missing '{key}'")
    if not isinstance(obj["matched"], list):
        raise GraderError("grader 'matched' must be a list")
    return obj


def aggregate(trials: list[TrialResult]) -> dict[str, float]:
    """Report spread, never just a mean.

    A mean alone hides the difference between a model that is reliably adequate
    and one that alternates between excellent and unusable — and the second is
    far more dangerous to route work to.
    """
    values = [t.score for t in trials]
    return {
        "mean": statistics.fmean(values),
        "min": min(values),
        "max": max(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def run_model(argv: list[str], prompt: str, timeout: int, env: Mapping[str, str]) -> str:
    try:
        proc = subprocess.run(
            [*argv, prompt],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=REPO_ROOT,
            env=dict(env),
        )
    except FileNotFoundError as exc:
        raise ConfigError(f"host CLI not found: {argv[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        raise GraderError(f"host timed out after {timeout}s") from exc
    blob = f"{proc.stdout}\n{proc.stderr}"
    if BLOCKED.search(blob):
        raise Blocked(blob.strip().splitlines()[0] if blob.strip() else "provider unavailable")
    if proc.returncode != 0:
        raise GraderError(f"host exited {proc.returncode}: {blob.strip()[:200]}")
    return proc.stdout


def host_argv(model: str | None) -> list[str]:
    """Separate argv elements: an opaque model id must never become shell syntax."""
    argv = ["claude", "-p", "--permission-mode", "dontAsk"]
    if model:
        argv[1:1] = ["--model", model]
    return argv


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", required=True)
    ap.add_argument("--model", help="host model; recorded in the result so a score is attributable")
    ap.add_argument("--trials", type=int, help="override the case trial count")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--dry-run", action="store_true", help="validate case, assets, and structure offline")
    args = ap.parse_args()

    try:
        case = load_case(args.case)
        # Deterministic kinds have no grader: lineage either held or it did
        # not, and inventing a semantic judgement over it would add noise to a
        # question that has an exact answer.
        graded = "grader" in case
        grader_prompt = read_asset(case, "grader") if graded else ""
        goal = read_asset(case, "goal_fixture") if case["kind"] == "draft-quality" else ""
    except ConfigError as exc:
        print(f"config error: {exc}", file=sys.stderr)
        return EXIT_CONFIG

    trials = args.trials or int(case["trials"])
    threshold = float(case.get("pass_threshold", 0.7))
    print(f"eval-planning: {case['id']}")
    print(f"  grader     : {case.get('grader_version', '(deterministic — no grader)')}")
    print(f"  host model : {args.model or '(host default)'}")
    print(f"  trials     : {trials}")

    if args.dry_run and case["kind"] == "brief-completeness":
        mandatory = case["mandatory_elements"]
        source = {e: f"value-for-{e}" for e in mandatory}
        if not check_brief_completeness(dict(source), source, mandatory).ok:
            print("dry-run: rejected a faithful brief", file=sys.stderr)
            return EXIT_FAIL
        dropped = {k: v for k, v in source.items() if k != "locked_decisions"}
        if check_brief_completeness(dropped, source, mandatory).ok:
            print("dry-run: accepted a brief missing a mandatory element", file=sys.stderr)
            return EXIT_FAIL
        tampered = dict(source, acceptance_criteria="quietly rewritten")
        altered = check_brief_completeness(tampered, source, mandatory)
        if altered.ok or "acceptance_criteria" not in altered.altered:
            print("dry-run: accepted a silently altered element", file=sys.stderr)
            return EXIT_FAIL
        punct = dict(source, acceptance_criteria="reject `--opaque; $(whoami)` verbatim")
        if not check_brief_completeness(punct, punct, mandatory).ok:
            print("dry-run: rejected legitimate punctuation as unsafe", file=sys.stderr)
            return EXIT_FAIL
        ctrl = dict(source, scenarios="line\x00truncated")
        if check_brief_completeness(ctrl, ctrl, mandatory).ok:
            print("dry-run: accepted a control character", file=sys.stderr)
            return EXIT_FAIL
        print(f"  mandatory  : {len(mandatory)} elements")
        print("  rejects missing and silently ALTERED elements")
        print("  keeps shell punctuation as content, refuses control characters")
        print("\ndry-run OK: completeness, fidelity, and value-safety checks are usable.")
        return EXIT_PASS

    if args.dry_run and case["kind"] == "packet-readiness":
        fields = case["required_fields"]
        markers = case["generic_markers"]
        good = {"title": "Add lease renewal", "acceptance_criteria": "renew extends lease_expires_at by TTL", "next_action": "Read src/engine/lease.zig renew()"}
        if not check_packet_readiness(good, fields, markers).ready:
            print("dry-run: rejected a ready packet", file=sys.stderr)
            return EXIT_FAIL
        generic = dict(good, acceptance_criteria="It is implemented and tested.")
        verdict = check_packet_readiness(generic, fields, markers)
        if verdict.ready:
            print("dry-run: accepted a generic packet", file=sys.stderr)
            return EXIT_FAIL
        missing = check_packet_readiness({"title": "x"}, fields, markers)
        if missing.ready or len(missing.reasons) < 2:
            print("dry-run: missing fields were not all named", file=sys.stderr)
            return EXIT_FAIL
        if check_digest_freshness("abc123", "def456").ready:
            print("dry-run: accepted a stale digest", file=sys.stderr)
            return EXIT_FAIL
        if check_no_claim_leak([{"entity_kind": "task", "entity_id": 7, "status": "active"}], 7).ready:
            print("dry-run: accepted a stranded claim", file=sys.stderr)
            return EXIT_FAIL
        print(f"  rejects generic  : {verdict.reasons[0]}")
        print(f"  rejects missing  : {len(missing.reasons)} field(s) named")
        print("  rejects stale digest and stranded claims")
        print("\ndry-run OK: readiness, digest freshness, and leak checks are usable.")
        return EXIT_PASS

    if args.dry_run and case["kind"] == "sentinel-lineage":
        entities = case["entities"]
        sentinels = mint_sentinels(entities, "dryrun")
        require = {e["slot"] for e in entities if e.get("must_carry_digest")}
        # Prove the checker REJECTS a misplaced sentinel, rather than only
        # proving it accepts a correct one — a checker that never fails is
        # indistinguishable from no checker.
        correct = {s.kind: [s.value] for s in sentinels}
        digests = {s.slot: "d" for s in sentinels}
        if not check_lineage(sentinels, correct, digests, require).ok:
            print("dry-run: checker rejected a correct lineage", file=sys.stderr)
            return EXIT_FAIL
        swapped = {sentinels[0].kind: [sentinels[1].value], sentinels[1].kind: [sentinels[0].value]}
        if check_lineage(sentinels, swapped, digests, require).ok:
            print("dry-run: checker accepted swapped placement", file=sys.stderr)
            return EXIT_FAIL
        if check_lineage(sentinels, correct, {}, require).ok:
            print("dry-run: checker accepted a missing digest", file=sys.stderr)
            return EXIT_FAIL
        redacted = redact_diagnostics(
            f"failed near {sentinels[0].value}: confidential source line here",
            sentinels,
            ["confidential source line here"],
        )
        if "confidential source line" in redacted or sentinels[0].value not in redacted:
            print("dry-run: redaction kept a source body or dropped a sentinel", file=sys.stderr)
            return EXIT_FAIL
        print(f"  sentinels  : {len(sentinels)} unique, digest-required: {len(require)}")
        print("  checker rejects swapped placement and missing digests")
        print("  diagnostics keep sentinels, drop source bodies")
        print("\ndry-run OK: lineage checker, digest requirement, and redaction are usable.")
        return EXIT_PASS

    if args.dry_run and case["kind"] == "seeded-recall":
        # Prove the manifest loads and that a shotgun review would be REJECTED,
        # rather than only proving the files parse.
        manifest = load_manifest(case)
        weights = case["severity_weights"]
        shotgun = score_seeded_recall(
            manifest,
            {
                "matched": [d["id"] for d in manifest],
                "unsupported_count": 40,
                "total_findings": 40 + len(manifest),
            },
            weights,
        )
        floor = float(case.get("precision_threshold", 0.5))
        if shotgun.precision >= floor:
            print("dry-run: a shotgun review was not rejected", file=sys.stderr)
            return EXIT_FAIL
        print(f"  defects    : {len(manifest)} ({', '.join(sorted({d['severity'] for d in manifest}))})")
        print(f"  shotgun review: recall {shotgun.recall:.2f} but precision "
              f"{shotgun.precision:.2f} < {floor:.2f} -> rejected")
        print("\ndry-run OK: manifest, grader, and anti-shotgun floor are usable.")
        return EXIT_PASS

    if args.dry_run:
        # Proves the case, fixture, grader, and structural gate are all usable
        # without spending a live call. A structurally invalid draft must fail
        # here, so the check is exercised against a known-bad sample.
        bad = check_structure("this is implemented and tested", case["structural"])
        if bad.ok:
            print("dry-run: structural gate accepted a known-bad draft", file=sys.stderr)
            return EXIT_FAIL
        # Every live run calls arena.assert_isolated(env, root) before the host
        # starts (decision D2). Prove the isolated env passes here, and that a
        # PLANAR_DB leaked outside the arena root is refused before spending a
        # live call.
        with tempfile.TemporaryDirectory(prefix="planar-eval-planning-dryrun-") as tmp:
            arena_root = Path(tmp)
            isolated_env = build_host_env(arena_root)
            leaked_env = dict(isolated_env)
            leaked_env["PLANAR_DB"] = str(Path.home() / ".planar" / "planar.db")
            try:
                arena.assert_isolated(leaked_env, arena_root)
            except arena.ArenaIsolationError:
                pass
            else:
                print("dry-run: accepted a PLANAR_DB leaked outside the arena", file=sys.stderr)
                return EXIT_FAIL
        print(f"  goal chars : {len(goal)}")
        print(f"  grader len : {len(grader_prompt)}")
        print(f"  structural gate rejects a known-bad draft: {'; '.join(bad.reasons())}")
        print("  isolation  : arena env resolves under root; leaked PLANAR_DB is refused")
        print("\ndry-run OK: case, fixture, grader, and structural gate are usable.")
        return EXIT_PASS

    if shutil.which("claude") is None:
        print("blocked: claude CLI not installed", file=sys.stderr)
        return EXIT_BLOCKED

    results: list[TrialResult] = []
    for i in range(1, trials + 1):
        draft_prompt = (
            "Draft planning specs for the goal below. Produce product-spec, tech-spec, "
            "test-spec, and roadmap content with Goal, Non-goals, Acceptance, and Open "
            "questions sections. Claim only what you actually did.\n\n"
            f"GOAL:\n{goal}"
        )
        # Each trial gets its own arena, isolation-checked before the host
        # starts (decision D2). Unlike the orchestrator harness (which has a
        # `--keep` flag and a failure-retention path), this planning harness
        # retains nothing: `TemporaryDirectory` deletes the arena the moment
        # the `with` block exits, win or lose, one arena per trial.
        with tempfile.TemporaryDirectory(prefix=f"planar-eval-planning-trial{i}-") as trial_dir:
            trial_root = Path(trial_dir)
            try:
                trial_env = build_host_env(trial_root)
                # This harness always spawns the claude CLI (host_argv);
                # staged config is not the same as authenticated — Keychain
                # login does not follow into a scratch CLAUDE_CONFIG_DIR
                # (Planar artifact 626 / task 6872). Fail closed before the
                # host spawns.
                arena.assert_vendor_auth(trial_env, "claude")
                draft = run_model(host_argv(args.model), draft_prompt, args.timeout, trial_env)
            except Blocked as exc:
                print(f"blocked: {exc}", file=sys.stderr)
                return EXIT_BLOCKED
            except (GraderError, ConfigError, arena.ArenaIsolationError, arena.VendorAuthError) as exc:
                print(f"trial {i}: host failure: {exc}", file=sys.stderr)
                return EXIT_FAIL

            structural = check_structure(draft, case["structural"])
            if not structural.ok:
                # Fail before paying for semantic grading.
                print(f"  trial {i}: STRUCTURAL FAIL — {'; '.join(structural.reasons())}")
                results.append(TrialResult(i, 0.0, {}, structural))
                continue

            try:
                raw = run_model(
                    host_argv(args.model),
                    f"{grader_prompt}\n\n--- DRAFT UNDER REVIEW ---\n{draft}",
                    args.timeout,
                    trial_env,
                )
                scores = parse_grader_output(raw)
            except Blocked as exc:
                print(f"blocked: {exc}", file=sys.stderr)
                return EXIT_BLOCKED
            except GraderError as exc:
                # Never a pass, never a silent skip.
                print(f"\nFAIL: grader error on trial {i}: {exc}", file=sys.stderr)
                return EXIT_FAIL

        score = statistics.fmean(scores.values())
        print(f"  trial {i}: {score:.2f}  " + " ".join(f"{k}={v:.2f}" for k, v in sorted(scores.items())))
        results.append(TrialResult(i, score, scores, structural))

    stats = aggregate(results)
    print(
        f"\nmean {stats['mean']:.2f}  min {stats['min']:.2f}  max {stats['max']:.2f}  "
        f"stdev {stats['stdev']:.2f}  (threshold {threshold:.2f})"
    )
    if stats["mean"] < threshold:
        print(f"FAIL: mean {stats['mean']:.2f} below threshold {threshold:.2f}")
        return EXIT_FAIL
    if stats["min"] < threshold:
        # Reported, not hidden by the mean: an unstable pass is a different
        # thing from a stable one and the operator should see which they have.
        print(f"PASS (unstable): worst trial {stats['min']:.2f} is below threshold.")
        return EXIT_PASS
    print("PASS")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
