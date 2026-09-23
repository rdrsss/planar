#!/usr/bin/env python3
"""Planar orchestrator evaluation harness.

The harness deliberately uses only the Python standard library. Repository
fixtures that emulate external CLIs remain executable shell test doubles.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

import arena
import vendors

ROOT = Path(__file__).resolve().parents[2]
CASES_DIR = ROOT / "evals" / "orchestrator" / "cases"
FIXTURES_DIR = ROOT / "evals" / "orchestrator" / "fixtures"
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")
# Textual fallback for hosts that report a limit as prose rather than as a
# structured event. Deliberately does NOT match the bare token `rate_limit`:
# the Claude CLI names its routine telemetry event `rate_limit_event` and emits
# it on every stream-json run, so matching that token blocks every run
# regardless of outcome. Structured status is authoritative when present.
RATE_LIMIT_TEXT_PATTERN = re.compile(
    r"session limit|api_error_status.?[:=].?429|rate limit (?:exceeded|reached)",
    re.IGNORECASE,
)
# The only `rate_limit_info.status` value that means "not limited". Any other
# value blocks, so an unrecognized status fails closed rather than silently
# grading a throttled run; the status is echoed into the reason string so a
# false block is self-diagnosing.
RATE_LIMIT_OK_STATUS = "allowed"
# `--permission-mode dontAsk` suppresses the prompt and denies anything not
# already permitted. The CLI auto-approves safe builtins like `echo`, but not
# `planar` — so without an explicit grant the orchestrator cannot run a single
# intake command and correctly refuses to render a preview it cannot ground.
# Canonical default lives in `vendors.py` (task 6849 split); these names stay
# for callers/tests that predate the split.
CLAUDE_LIVE_ALLOWED_TOOLS = ",".join(vendors.DEFAULT_ALLOWED_TOOLS)
# Passed as ONE argv element. `--allowedTools` is variadic, so the two-token
# form swallows the prompt positional that follows it and the CLI dies with
# "Input must be provided ... when using --print".
CLAUDE_LIVE_ALLOWED_TOOLS_ARG = vendors.allowed_tools_arg(vendors.DEFAULT_ALLOWED_TOOLS)


def rate_limit_reason(text: str) -> str | None:
    """Return a reason when the host actually rate-limited the run, else None.

    The Claude CLI emits a `rate_limit_event` line on every stream-json run,
    including when nothing is limited (`rate_limit_info.status == "allowed"`).
    Read that structured status rather than substring-matching the event name.
    """
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("{") or "rate_limit_event" not in line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            # Unreadable telemetry proves nothing either way; let the textual
            # scan below decide instead of inventing a verdict from a
            # half-written line.
            continue
        if not isinstance(event, dict) or event.get("type") != "rate_limit_event":
            continue
        info = event.get("rate_limit_info")
        if not isinstance(info, dict):
            continue
        status = str(info.get("status", "")).strip().lower()
        if status and status != RATE_LIMIT_OK_STATUS:
            kind = info.get("rateLimitType") or "unspecified"
            return f"host rate limit ({kind}: {status})"
    if RATE_LIMIT_TEXT_PATTERN.search(text):
        return "host rate limit"
    return None


class EvalFailure(RuntimeError):
    """A failed evaluation with optional retained artifacts."""

    def __init__(self, message: str, artifacts: Path | None = None):
        super().__init__(message)
        self.artifacts = artifacts


class EvalBlocked(RuntimeError):
    """An evaluation blocked by external capacity or credentials."""

    def __init__(self, message: str, artifacts: Path | None = None):
        super().__init__(message)
        self.artifacts = artifacts


class SchemaValidationError(EvalFailure):
    """A case document violates `evals/orchestrator/schema.json`, or the
    schema itself uses a construct `validate_against_json_schema` does not
    understand (task 6891). The two are both hard errors: a schema keyword
    the checker cannot interpret must never be silently treated as
    satisfied, so an editor who widens `schema.json` past this subset
    finds out at the next `load_cases()` rather than losing enforcement
    quietly.
    """


# `schema.json` was documentary only until task 6891: nothing loaded it, so
# a `required` edit there was prose, not enforcement. This is the on-disk
# schema `validate_case` now checks every case against.
CASE_SCHEMA_PATH = ROOT / "evals" / "orchestrator" / "schema.json"

# Schema keywords `validate_against_json_schema` understands. Split into
# metadata (carries no constraint; safe to ignore) and constraint keywords
# (interpreted below) so the "unsupported construct" check has a single
# source of truth for what this minimal, stdlib-only subset actually
# implements -- exactly the constructs `schema.json` uses today: type,
# required, properties, enum, items, plus const/pattern/minLength/
# minItems/uniqueItems/minimum/maximum/maxProperties/additionalProperties/
# allOf/if-then/contains/not, which schema.json also uses.
_SCHEMA_METADATA_KEYWORDS = {"$schema", "$id", "title", "description", "default"}
_SCHEMA_CONSTRAINT_KEYWORDS = {
    "type",
    "required",
    "properties",
    "additionalProperties",
    "enum",
    "const",
    "pattern",
    "minLength",
    "items",
    "minItems",
    "uniqueItems",
    "minimum",
    "maximum",
    "exclusiveMinimum",
    "maxProperties",
    "allOf",
    "if",
    "then",
    "contains",
    "not",
}
_SCHEMA_KNOWN_KEYWORDS = _SCHEMA_METADATA_KEYWORDS | _SCHEMA_CONSTRAINT_KEYWORDS

_SCHEMA_CACHE: dict[str, Any] | None = None


def load_case_schema() -> dict[str, Any]:
    """Read and cache `schema.json`. A module-level cache (rather than a
    read on every case) keeps `load_cases()` from re-parsing the same file
    once per case file on disk.
    """
    global _SCHEMA_CACHE
    if _SCHEMA_CACHE is None:
        _SCHEMA_CACHE = read_json(CASE_SCHEMA_PATH)
    return _SCHEMA_CACHE


def _json_type_name(value: Any) -> str:
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    if value is None:
        return "null"
    return "unknown"  # pragma: no cover - every JSON value matches above


def _matches_schema_type(value: Any, expected: str) -> bool:
    actual = _json_type_name(value)
    if expected == "number":
        return actual in ("number", "integer")
    return actual == expected


def validate_against_json_schema(
    schema: Any, instance: Any, *, path: str = "$"
) -> None:
    """Validate `instance` against `schema` using a minimal, stdlib-only
    JSON-Schema subset (task 6891): `type`, `required`, `properties`,
    `additionalProperties`, `enum`, `const`, `pattern`, `minLength`,
    `items`, `minItems`, `uniqueItems`, `minimum`, `maximum`,
    `maxProperties`, `allOf`, `if`/`then`, `contains`, and `not` -- exactly
    the constructs `evals/orchestrator/schema.json` uses. A schema object
    carrying any OTHER keyword raises `SchemaValidationError` naming it
    rather than silently passing: this checker never treats an
    unrecognized construct as satisfied.

    Raises `SchemaValidationError` on the first violation found (a
    structural mismatch, or an unsupported keyword), never returns a
    boolean.
    """
    if not isinstance(schema, dict):
        return
    unsupported = sorted(set(schema) - _SCHEMA_KNOWN_KEYWORDS)
    if unsupported:
        raise SchemaValidationError(
            f"{path}: schema uses unsupported construct(s): "
            + ", ".join(unsupported)
        )

    if "type" in schema:
        expected = schema["type"]
        expected_types = expected if isinstance(expected, list) else [expected]
        if not any(_matches_schema_type(instance, t) for t in expected_types):
            raise SchemaValidationError(
                f"{path}: expected type {expected!r}, got "
                f"{_json_type_name(instance)}"
            )

    if "const" in schema and instance != schema["const"]:
        raise SchemaValidationError(
            f"{path}: expected const {schema['const']!r}, got {instance!r}"
        )

    if "enum" in schema and instance not in schema["enum"]:
        raise SchemaValidationError(
            f"{path}: value {instance!r} is not one of {schema['enum']!r}"
        )

    if "pattern" in schema and isinstance(instance, str):
        if re.search(schema["pattern"], instance) is None:
            raise SchemaValidationError(
                f"{path}: {instance!r} does not match pattern "
                f"{schema['pattern']!r}"
            )

    if "minLength" in schema and isinstance(instance, str):
        if len(instance) < schema["minLength"]:
            raise SchemaValidationError(
                f"{path}: length {len(instance)} is below minLength "
                f"{schema['minLength']}"
            )

    if (
        isinstance(instance, (int, float))
        and not isinstance(instance, bool)
        and ("minimum" in schema or "maximum" in schema or "exclusiveMinimum" in schema)
    ):
        if "minimum" in schema and instance < schema["minimum"]:
            raise SchemaValidationError(
                f"{path}: {instance} is below minimum {schema['minimum']}"
            )
        if "maximum" in schema and instance > schema["maximum"]:
            raise SchemaValidationError(
                f"{path}: {instance} exceeds maximum {schema['maximum']}"
            )
        if "exclusiveMinimum" in schema and instance <= schema["exclusiveMinimum"]:
            raise SchemaValidationError(
                f"{path}: {instance} does not exceed exclusiveMinimum "
                f"{schema['exclusiveMinimum']}"
            )

    if isinstance(instance, dict):
        if "required" in schema:
            missing = [key for key in schema["required"] if key not in instance]
            if missing:
                raise SchemaValidationError(
                    f"{path}: missing required key(s): {', '.join(missing)}"
                )
        if "maxProperties" in schema and len(instance) > schema["maxProperties"]:
            raise SchemaValidationError(
                f"{path}: object has {len(instance)} properties, exceeds "
                f"maxProperties {schema['maxProperties']}"
            )
        properties = schema.get("properties")
        if isinstance(properties, dict):
            for key, subschema in properties.items():
                if key in instance:
                    validate_against_json_schema(
                        subschema, instance[key], path=f"{path}.{key}"
                    )
        additional = schema.get("additionalProperties")
        allowed = set(properties) if isinstance(properties, dict) else set()
        if additional is False:
            extra = sorted(set(instance) - allowed)
            if extra:
                raise SchemaValidationError(
                    f"{path}: unexpected additional propert"
                    f"{'y' if len(extra) == 1 else 'ies'}: {', '.join(extra)}"
                )
        elif isinstance(additional, dict):
            for key in sorted(set(instance) - allowed):
                validate_against_json_schema(
                    additional, instance[key], path=f"{path}.{key}"
                )

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            raise SchemaValidationError(
                f"{path}: array has {len(instance)} items, below minItems "
                f"{schema['minItems']}"
            )
        if "uniqueItems" in schema and schema["uniqueItems"]:
            seen: list[Any] = []
            for item in instance:
                if item in seen:
                    raise SchemaValidationError(f"{path}: array items are not unique")
                seen.append(item)
        items_schema = schema.get("items")
        if isinstance(items_schema, dict):
            for index, item in enumerate(instance):
                validate_against_json_schema(
                    items_schema, item, path=f"{path}[{index}]"
                )
        if "contains" in schema:
            contains_schema = schema["contains"]
            matched = False
            for item in instance:
                try:
                    validate_against_json_schema(contains_schema, item, path=path)
                except SchemaValidationError:
                    continue
                matched = True
                break
            if not matched:
                raise SchemaValidationError(f"{path}: no item satisfies 'contains'")

    if "not" in schema:
        try:
            validate_against_json_schema(schema["not"], instance, path=path)
        except SchemaValidationError:
            pass
        else:
            raise SchemaValidationError(
                f"{path}: instance must not validate against 'not' schema"
            )

    if "if" in schema:
        try:
            validate_against_json_schema(schema["if"], instance, path=path)
        except SchemaValidationError:
            condition_met = False
        else:
            condition_met = True
        if condition_met and "then" in schema:
            validate_against_json_schema(schema["then"], instance, path=path)

    if "allOf" in schema:
        for subschema in schema["allOf"]:
            validate_against_json_schema(subschema, instance, path=path)


@dataclass
class Options:
    mode: str
    vendor: str
    surface: str
    case_filter: str | None
    results_dir: Path | None
    keep: bool
    # Task 6852 (spend ceiling): suite-level cost ceiling and the running
    # per-run cost total accumulated across every case run through this same
    # `Options` instance in one `main()` invocation.
    max_usd: float | None = None
    suite_spend_usd: list[float] = field(default_factory=list)
    # Task 6859 (trials): how many times to repeat each selected live/
    # lifecycle case. 1 (the default) preserves the pre-6859 single-run
    # behavior exactly.
    trials: int = 1


@dataclass
class LifecycleContext:
    case_path: Path
    case: dict[str, Any]
    artifacts: Path
    repo: Path
    env: dict[str, str]
    plan_id: str
    task_id: str


def pass_line(message: str) -> None:
    print(f"PASS: {message}", flush=True)


# One `(case_id, status, message)` tuple per non-passing unit, where
# `status` is `"fail"` or `"blocked"`. A passing unit contributes nothing
# (C6 / task 6840: the aggregate report only needs to name what went
# wrong).
CaseOutcome = tuple[str, str, str]


def collect_case_failures(
    entries: list[tuple[Path, dict[str, Any]]],
    run_one: Callable[[Path, dict[str, Any]], None],
) -> list[CaseOutcome]:
    """Run `run_one` for every `(path, case)` entry without stopping at the
    first failure (C6 / task 6840). Returns one outcome per case that
    raised `EvalFailure` or `EvalBlocked`; a case that raises neither is
    simply not represented in the result, so all N graders still execute
    even when earlier ones fail.
    """
    outcomes: list[CaseOutcome] = []
    for path, case in entries:
        try:
            run_one(path, case)
        except EvalBlocked as exc:
            outcomes.append((case["id"], "blocked", str(exc)))
        except EvalFailure as exc:
            outcomes.append((case["id"], "fail", str(exc)))
    return outcomes


def emit_aggregate_report(
    outcomes: list[CaseOutcome], *, total: int, unit: str, pass_message: str
) -> None:
    """Print the C6 aggregate report for a batch of `total` attempted
    units given only the non-passing `outcomes`. Every unit passing prints
    `pass_message` via `pass_line` unchanged from the pre-aggregation
    behaviour (single-case `--case` runs keep this path); any failing or
    blocked unit instead prints one `FAIL SUMMARY: N of M <unit> failed`
    header followed by one line per non-passing unit, so an operator sees
    every failure from a single invocation rather than only the first.
    """
    if not outcomes:
        pass_line(pass_message)
        return
    print(f"FAIL SUMMARY: {len(outcomes)} of {total} {unit} failed", file=sys.stderr)
    for case_id, status, message in outcomes:
        print(f"  {status}: {case_id}: {message}", file=sys.stderr)


def batch_exit_code(outcomes: list[CaseOutcome]) -> int:
    """Precedence for a batch of `CaseOutcome`s (C6 / task 6840): any hard
    failure wins over any block, which wins over an all-pass batch. A
    blocked-but-not-failed batch still exits 75 so the distinct blocked
    exit code (see `cli_main`) survives aggregation.
    """
    if any(status == "fail" for _, status, _ in outcomes):
        return 1
    if any(status == "blocked" for _, status, _ in outcomes):
        return 75
    return 0


def write_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def write_json(path: Path, value: Any) -> None:
    write_text(path, json.dumps(value, indent=2, sort_keys=False) + "\n")


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvalFailure(f"invalid JSON file {path}: {exc}") from exc


def command_exists(name: str, env: dict[str, str] | None = None) -> bool:
    return shutil.which(name, path=(env or os.environ).get("PATH")) is not None


def require_commands(names: Iterable[str], env: dict[str, str] | None = None) -> None:
    for name in names:
        if not command_exists(name, env):
            raise EvalFailure(f"missing command: {name}")


def run_command(
    args: Sequence[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    check: bool = True,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    run_options: dict[str, Any] = {
        "cwd": cwd,
        "env": env,
        "text": True,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "check": False,
    }
    if input_text is None:
        run_options["stdin"] = subprocess.DEVNULL
    else:
        run_options["input"] = input_text
    result = subprocess.run(list(args), **run_options)
    if check and result.returncode != 0:
        rendered = " ".join(args)
        detail = result.stderr.strip() or result.stdout.strip()
        raise EvalFailure(f"command failed ({result.returncode}): {rendered}\n{detail}")
    return result


def run_json(
    args: Sequence[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> Any:
    result = run_command(args, cwd=cwd, env=env)
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise EvalFailure(f"command returned invalid JSON: {' '.join(args)}") from exc


def run_passthrough(
    args: Sequence[str], *, cwd: Path | None = None, env: dict[str, str] | None = None
) -> None:
    result = subprocess.run(list(args), cwd=cwd, env=env, check=False)
    if result.returncode != 0:
        raise EvalFailure(f"command failed ({result.returncode}): {' '.join(args)}")


def run_to_file(
    args: Sequence[str],
    output: Path,
    *,
    cwd: Path,
    env: dict[str, str],
    timeout_seconds: int,
) -> int:
    """Run a host command with process-group timeout and combined JSONL output."""

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as stream:
        process = subprocess.Popen(
            list(args),
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=stream,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            return process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            return 124


def jsonl_objects(path: Path) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    if not path.exists():
        return objects
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            objects.append(value)
    return objects


def nested_strings(value: Any) -> Iterable[str]:
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for child in value.values():
            yield from nested_strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from nested_strings(child)


def extract_host_transcript(raw: Path, vendor: str, artifact_dir: Path) -> str:
    objects = jsonl_objects(raw)
    all_strings = [text for obj in objects for text in nested_strings(obj)]
    transcript = "\n".join(all_strings)
    write_text(artifact_dir / "transcript.txt", transcript + ("\n" if transcript else ""))

    finals: list[str] = []
    if vendor == "codex":
        for obj in objects:
            item = obj.get("item")
            if (
                obj.get("type") == "item.completed"
                and isinstance(item, dict)
                and item.get("type") == "agent_message"
                and isinstance(item.get("text"), str)
            ):
                finals.append(item["text"])
    else:
        for obj in objects:
            if obj.get("type") == "result" and isinstance(obj.get("result"), str):
                finals.append(obj["result"])
    final = finals[-1] if finals else ""
    write_text(artifact_dir / "final.txt", final + ("\n" if final else ""))
    return final


class EvalOverBudget(EvalFailure):
    """A case's or the suite's spend ceiling was exceeded (task 6852)."""


def resolve_allowed_tools(section: dict[str, Any] | None) -> tuple[str, ...]:
    """Resolve the per-case Claude tool allowlist (task 6850).

    `section` is `case["live"]` or `case["lifecycle"]`; a case that does not
    declare `allowed_tools` gets `vendors.DEFAULT_ALLOWED_TOOLS`.
    """
    if not section:
        return vendors.DEFAULT_ALLOWED_TOOLS
    tools = section.get("allowed_tools")
    if not tools:
        return vendors.DEFAULT_ALLOWED_TOOLS
    return tuple(tools)


def extract_usage(raw: Path, vendor: str) -> dict[str, Any]:
    """Extract calls-per-task and token/cost usage from a host transcript
    (task 6859). Raises `EvalFailure` when the transcript carries no
    recognizable usage block at all -- a transcript that never reports usage
    is a harness/host defect, not a zero-cost run, so this never silently
    returns zeros.

    Claude's stream-json protocol reports usage/cost on its terminal
    `type: "result"` event (Planar artifact 622). Codex's `exec --json`
    protocol does not publish a cost figure at all; `total_cost_usd` stays
    `None` for codex runs and only the token counters are populated, so
    codex-side spend-ceiling enforcement is necessarily token-based, not
    cost-based (see `enforce_spend_ceiling`).
    """
    objects = jsonl_objects(raw)
    calls = 0
    input_tokens = 0
    output_tokens = 0
    total_cost_usd: float | None = None
    if vendor == "claude":
        for obj in objects:
            if obj.get("type") != "result":
                continue
            calls += 1
            cost = obj.get("total_cost_usd")
            if isinstance(cost, (int, float)) and not isinstance(cost, bool):
                total_cost_usd = (total_cost_usd or 0.0) + float(cost)
            usage = obj.get("usage")
            if isinstance(usage, dict):
                input_tokens += int(usage.get("input_tokens") or 0)
                output_tokens += int(usage.get("output_tokens") or 0)
    else:
        for obj in objects:
            candidate = obj if obj.get("type") == "token_count" else obj.get("msg")
            if not isinstance(candidate, dict) or candidate.get("type") != "token_count":
                continue
            calls += 1
            input_tokens += int(candidate.get("input_tokens") or 0)
            output_tokens += int(candidate.get("output_tokens") or 0)
    if calls == 0:
        raise EvalFailure(
            f"host transcript carries no usage block for vendor={vendor}: {raw}"
        )
    return {
        "calls": calls,
        "total_cost_usd": total_cost_usd,
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
    }


def merge_usage(accumulated: dict[str, Any], turn: dict[str, Any]) -> dict[str, Any]:
    """Fold one turn's `extract_usage` result into a running total."""
    cost = accumulated.get("total_cost_usd")
    turn_cost = turn.get("total_cost_usd")
    if turn_cost is not None:
        cost = (cost or 0.0) + turn_cost
    return {
        "calls": accumulated.get("calls", 0) + turn["calls"],
        "total_cost_usd": cost,
        "input_tokens": accumulated.get("input_tokens", 0) + turn["input_tokens"],
        "output_tokens": accumulated.get("output_tokens", 0) + turn["output_tokens"],
    }


def enforce_spend_ceiling(
    *,
    artifact_dir: Path,
    case_id: str,
    options: Options,
    repo: Path,
    env: dict[str, str],
    plan_id: str,
    usage: dict[str, Any],
    case_budget_usd: float | None,
) -> None:
    """Abort the run if the case or suite spend ceiling was exceeded
    (task 6852). Cost enforcement is skipped when the vendor reports no
    cost figure (codex today -- see `extract_usage`); the case still gets
    graded normally in that case.

    On over-budget: writes `grade.json` with `status: "over-budget"`,
    reconciles the arena's own claims to zero active via
    `planar-agent reconcile --plan <plan_id>` (never the operator's
    database -- `env`/`repo` are always the scratch arena), and raises
    `EvalOverBudget` with the artifacts retained (the caller's exception
    handler attaches `artifact_dir` the same way every other fail path
    does; this function never deletes anything itself).
    """
    cost = usage.get("total_cost_usd")
    if cost is None:
        return
    options.suite_spend_usd.append(cost)
    suite_total = sum(options.suite_spend_usd)
    over_case = case_budget_usd is not None and cost > case_budget_usd
    over_suite = options.max_usd is not None and suite_total > options.max_usd
    if not (over_case or over_suite):
        return
    if over_case:
        reason = f"run cost ${cost:.4f} exceeds case budget.max_usd ${case_budget_usd:.4f}"
    else:
        reason = f"suite spend ${suite_total:.4f} exceeds --max-usd ${options.max_usd:.4f}"
    write_grade(artifact_dir, "over-budget", case_id, options, reason, usage=usage)
    # The ledger has to carry this too, not just grade.json: a run that
    # stopped because it hit a ceiling is exactly the kind of result an
    # operator needs to see when asking "has this case ever passed?"
    # (task 6852 + 6860; reviewer, M3 cycle 1).
    if options.mode in {"live", "lifecycle"}:
        record_ledger_row({"id": case_id}, artifact_dir, options, "over-budget")
    try:
        run_command(["planar-agent", "reconcile", "--plan", plan_id], cwd=repo, env=env)
    except Exception:
        # Reconciliation is best-effort cleanup; the over-budget failure
        # itself must still surface even if the reconcile call fails.
        pass
    raise EvalOverBudget(f"{case_id}: {reason}", artifact_dir)


def extract_session_id(raw: Path, vendor: str) -> str:
    for obj in jsonl_objects(raw):
        if vendor == "codex" and obj.get("type") == "thread.started":
            value = obj.get("thread_id")
            if isinstance(value, str):
                return value
        if vendor == "claude":
            value = obj.get("session_id")
            if isinstance(value, str):
                return value
    return ""


def load_cases() -> list[tuple[Path, dict[str, Any]]]:
    loaded: list[tuple[Path, dict[str, Any]]] = []
    seen: set[str] = set()
    for path in sorted(CASES_DIR.glob("*.json")):
        case = read_json(path)
        validate_case(path, case)
        case_id = case["id"]
        if case_id in seen:
            raise EvalFailure(f"duplicate case id: {case_id}")
        seen.add(case_id)
        loaded.append((path, case))
    if not loaded:
        raise EvalFailure(f"no orchestrator cases found in {CASES_DIR}")
    return loaded


def validate_case(path: Path, case: Any) -> None:
    def invalid(reason: str) -> None:
        raise EvalFailure(f"invalid case definition {path}: {reason}")

    if not isinstance(case, dict):
        invalid("root must be an object")
    try:
        validate_against_json_schema(load_case_schema(), case)
    except SchemaValidationError as exc:
        invalid(f"schema.json violation: {exc}")
    if case.get("schema_version") != 1 or case.get("skill") != "orchestrator":
        invalid("unsupported schema_version or skill")
    case_id = case.get("id")
    if not isinstance(case_id, str) or not ID_PATTERN.fullmatch(case_id):
        invalid("id must be lowercase kebab-case")
    if not isinstance(case.get("description"), str) or not case["description"]:
        invalid("description is required")
    tags = case.get("tags")
    tiers = case.get("tiers")
    assertions = case.get("contract_assertions")
    if not isinstance(tags, list) or not tags:
        invalid("tags must be a non-empty array")
    if (
        not isinstance(tiers, list)
        or not tiers
        or any(tier not in {"contract", "live", "lifecycle"} for tier in tiers)
    ):
        invalid("tiers contain an unsupported value")
    if not isinstance(assertions, list) or not assertions:
        invalid("contract_assertions must be a non-empty array")
    assertion_ids: set[str] = set()
    for assertion in assertions:
        if not isinstance(assertion, dict):
            invalid("assertion must be an object")
        assertion_id = assertion.get("id")
        if not isinstance(assertion_id, str) or not ID_PATTERN.fullmatch(assertion_id):
            invalid("assertion id must be lowercase kebab-case")
        if assertion_id in assertion_ids:
            invalid(f"duplicate assertion id: {assertion_id}")
        assertion_ids.add(assertion_id)
        if assertion.get("mode") not in {"all", "any", "none"}:
            invalid(f"{assertion_id} has unsupported mode")
        if not isinstance(assertion.get("description"), str) or not assertion["description"]:
            invalid(f"{assertion_id} requires a description")
        paths = assertion.get("paths")
        if not isinstance(paths, list) or not paths or not all(
            isinstance(item, str) and item for item in paths
        ):
            invalid(f"{assertion_id} requires paths")
        if not isinstance(assertion.get("pattern"), str) or not assertion["pattern"]:
            invalid(f"{assertion_id} requires a pattern")
    expected = case.get("expected")
    if not isinstance(expected, dict):
        invalid("expected must be an object")
    if "live" not in tiers and "lifecycle" not in tiers and expected:
        invalid("contract-only cases cannot declare semantic expected state")
    if "live" in tiers:
        live = case.get("live")
        tasks = case.get("tasks")
        if not isinstance(live, dict) or live.get("scenario") != "phase3-preview":
            invalid("live phase3-preview adapter is required")
        if not isinstance(live.get("prompt"), str) or not live["prompt"]:
            invalid("live prompt is required")
        models = live.get("expected_models")
        if not isinstance(models, dict) or not all(
            isinstance(models.get(vendor), str) and models[vendor]
            for vendor in ("codex", "claude")
        ):
            invalid("live expected_models must define codex and claude")
        if not isinstance(tasks, list) or not tasks:
            invalid("live cases require tasks")
        timeout = live.get("timeout_seconds", 300)
        if not isinstance(timeout, int) or not 30 <= timeout <= 3600:
            invalid("live timeout_seconds must be 30..3600")
        try:
            vendors.validate_allowed_tools(resolve_allowed_tools(live))
        except vendors.UnknownToolError as exc:
            invalid(f"live allowed_tools names an unknown tool: {exc.tool}")
    if "lifecycle" in tiers:
        lifecycle = case.get("lifecycle")
        setup = case.get("setup")
        tasks = case.get("tasks")
        if (
            not isinstance(lifecycle, dict)
            or lifecycle.get("scenario") != "controlled-classic"
            or lifecycle.get("surface") != "agent"
            or not isinstance(lifecycle.get("prompt"), str)
            or not lifecycle["prompt"]
        ):
            invalid("controlled-classic lifecycle adapter is required")
        if (
            not isinstance(setup, dict)
            or setup.get("fixture") != "controlled-classic"
            or not isinstance(setup.get("specialist_scenario"), str)
        ):
            invalid("controlled lifecycle fixture setup is required")
        if not isinstance(tasks, list) or len(tasks) != 1:
            invalid("lifecycle cases require exactly one task")
        try:
            vendors.validate_allowed_tools(resolve_allowed_tools(lifecycle))
        except vendors.UnknownToolError as exc:
            invalid(f"lifecycle allowed_tools names an unknown tool: {exc.tool}")
        forbidden_events = expected.get("forbidden_events", [])
        unemittable = [
            name for name in forbidden_events if name not in EMITTABLE_EVENTS
        ]
        if unemittable:
            invalid(
                "forbidden_events name an event no fixture path can emit: "
                + ", ".join(sorted(unemittable))
            )
        post_state = expected.get("post_state")
        if not isinstance(post_state, dict):
            invalid("lifecycle cases require expected.post_state")
        missing_keys = [
            key
            for key in ("task_status", "file_value")
            if not post_state.get(key)
        ]
        if missing_keys:
            invalid(
                "lifecycle cases require expected.post_state to set: "
                + ", ".join(missing_keys)
            )


def select_cases(
    cases: list[tuple[Path, dict[str, Any]]], selector: str | None
) -> list[tuple[Path, dict[str, Any]]]:
    if selector is None:
        return cases
    path = Path(selector)
    if path.is_file():
        resolved = path.resolve()
        case = read_json(resolved)
        validate_case(resolved, case)
        return [(resolved, case)]
    matches = [entry for entry in cases if entry[1]["id"] == selector]
    if len(matches) != 1:
        raise EvalFailure(f"unknown or ambiguous case id: {selector}")
    return matches


def python_pattern(pattern: str) -> str:
    return pattern.replace("[[:space:]]", r"\s")


def regex_search(pattern: str, text: str, *, multiline: bool = False) -> bool:
    flags = re.MULTILINE | (re.DOTALL if multiline else 0)
    try:
        return re.search(python_pattern(pattern), text, flags) is not None
    except re.error as exc:
        raise EvalFailure(f"invalid evaluation regex {pattern!r}: {exc}") from exc


def grade_contract(case_path: Path, case: dict[str, Any], root: Path = ROOT) -> None:
    case_id = case["id"]
    for assertion in case["contract_assertions"]:
        assertion_id = assertion["id"]
        matched = 0
        paths = assertion["paths"]
        for relative in paths:
            path = root / relative
            if not path.is_file():
                raise EvalFailure(
                    f"{case_id}/{assertion_id} references missing path: {relative}"
                )
            text = path.read_text(encoding="utf-8")
            if regex_search(
                assertion["pattern"],
                text,
                multiline=bool(assertion.get("multiline", False)),
            ):
                matched += 1
        mode = assertion["mode"]
        total = len(paths)
        okay = (
            (mode == "all" and matched == total)
            or (mode == "any" and matched > 0)
            or (mode == "none" and matched == 0)
        )
        if not okay:
            if mode == "any":
                detail = "matched no paths"
            elif mode == "none":
                detail = f"matched {matched} forbidden paths"
            else:
                detail = f"matched {matched} of {total} paths"
            raise EvalFailure(
                f"{case_id}/{assertion_id}: {assertion['description']} ({detail})"
            )
        pass_line(f"{case_id}/{assertion_id}")


def grade_coherence(root: Path = ROOT) -> None:
    core_rel = [
        "skills/src/pl-orchestrator.md",
        "agents/orchestrator.md",
        "agents/methodology.md",
        "agents/doctrine.md",
        "skills/src/pl-coder.md",
        "agents/coder.md",
        "skills/src/pl-reviewer.md",
        "agents/reviewer.md",
        "skills/src/pl-test-coder.md",
        "agents/test-coder.md",
        "agents/janitor.md",
    ]
    for relative in core_rel:
        if not (root / relative).is_file():
            raise EvalFailure(f"orchestrator-coherence: missing core contract: {relative}")

    budgets = {
        # 2500 in the armarium era; Planar's surface lint requires the seven
        # literal feedback H2 sections on every user-invocable skill, which
        # costs ~100 words of structural envelope over the condensed form the
        # original budget was calibrated against.
        # 2650->2800: task 6492 / PR #181 grew pl-orchestrator.md past 2650;
        # plan 1065 M4 slug hh-drop-word-budgets removes these budgets
        # entirely, so this is a bump to unblock, not a re-calibration.
        "skills/src/pl-orchestrator.md": 2800,
        "agents/orchestrator.md": 5500,
        "agents/coder.md": 2200,
        "agents/reviewer.md": 2200,
        "skills/src/pl-coder.md": 1800,
        "skills/src/pl-reviewer.md": 1800,
        "agents/test-coder.md": 1400,
        "agents/janitor.md": 1400,
        "skills/src/pl-test-coder.md": 800,
        "skills/src/pl-research.md": 800,
    }
    for relative, limit in budgets.items():
        count = len((root / relative).read_text(encoding="utf-8").split())
        if count > limit:
            raise EvalFailure(
                f"orchestrator-coherence: {relative} exceeds its executable "
                f"prompt budget ({count} > {limit} words)"
            )

    def forbid(pattern: str, relatives: Sequence[str], *, ignore_case: bool = True) -> None:
        flags = re.MULTILINE | (re.IGNORECASE if ignore_case else 0)
        compiled = re.compile(pattern, flags)
        hits: list[str] = []
        for relative in relatives:
            for number, line in enumerate(
                (root / relative).read_text(encoding="utf-8").splitlines(), 1
            ):
                if compiled.search(line):
                    hits.append(f"{relative}:{number}:{line}")
        if hits:
            raise EvalFailure(
                "orchestrator-coherence: forbidden contract text found:\n"
                + "\n".join(hits)
            )

    def must(pattern: str, relatives: Sequence[str]) -> None:
        compiled = re.compile(pattern, re.IGNORECASE | re.MULTILINE | re.DOTALL)
        for relative in relatives:
            text = (root / relative).read_text(encoding="utf-8")
            if not compiled.search(text):
                raise EvalFailure(
                    f"orchestrator-coherence: {relative} is missing required "
                    f"contract: {pattern}"
                )

    forbid(
        r"\b(zig|golang|rust|python|typescript|javascript|ruby)\b|\.zig\b",
        core_rel,
    )
    forbid(
        r"make (fmt-check|build|test|test-integration)|scriptorium check -config",
        core_rel,
    )
    forbid(
        r"recommended for:.*(mechanical|docs-polish|single-verb)",
        ["skills/src/pl-orchestrator.md", "agents/orchestrator.md", "agents/methodology.md"],
    )
    forbid(r"planar task done", ["skills/src/pl-coder.md", "agents/coder.md"])

    must(
        r"Planar-managed Git repositor",
        ["skills/src/pl-orchestrator.md", "agents/orchestrator.md", "agents/janitor.md"],
    )
    must(
        r"validation profile",
        [
            "skills/src/pl-orchestrator.md",
            "agents/orchestrator.md",
            "agents/methodology.md",
            "skills/src/pl-coder.md",
            "agents/coder.md",
            "skills/src/pl-reviewer.md",
            "agents/reviewer.md",
            "skills/src/pl-test-coder.md",
            "agents/test-coder.md",
        ],
    )
    must(
        r"structured.*evidence|evidence row",
        [
            "skills/src/pl-coder.md",
            "agents/coder.md",
            "skills/src/pl-reviewer.md",
            "agents/reviewer.md",
            "skills/src/pl-test-coder.md",
            "agents/test-coder.md",
        ],
    )
    must(r"pl-spec-review", ["skills/src/pl-orchestrator.md", "agents/orchestrator.md"])
    must(
        r"never recommended",
        ["skills/src/pl-orchestrator.md", "agents/orchestrator.md", "agents/methodology.md"],
    )
    must(
        r"in-pwd.*barrel-bypass|barrel-bypass.*in-pwd",
        [
            "skills/src/pl-coder.md",
            "agents/coder.md",
            "skills/src/pl-orchestrator.md",
            "agents/orchestrator.md",
        ],
    )
    must(
        r"github-pr.*external-pr.*local-ref.*already-integrated",
        ["agents/janitor.md"],
    )
    must(
        r"docs_outcome: not-configured",
        ["skills/src/pl-orchestrator.md", "agents/orchestrator.md"],
    )
    must(
        r"coder.*(narrative )?report|coder's report",
        ["skills/src/pl-reviewer.md", "agents/reviewer.md", "agents/methodology.md"],
    )
    must(r"Self-contained output contract", ["skills/src/pl-research.md"])
    must(r"fork_turns=none", ["evals/orchestrator/harness.py"])
    must(r"complete final response verbatim", ["evals/orchestrator/harness.py"])


def grade_coherence_negative_control() -> None:
    with tempfile.TemporaryDirectory(prefix="planar-coherence-negative.") as raw_tmp:
        temp_root = Path(raw_tmp)
        for directory in ("skills", "agents"):
            shutil.copytree(ROOT / directory, temp_root / directory)
        harness_target = temp_root / "evals" / "orchestrator" / "harness.py"
        harness_target.parent.mkdir(parents=True)
        shutil.copy2(Path(__file__), harness_target)
        with (temp_root / "skills" / "src" / "pl-orchestrator.md").open(
            "a", encoding="utf-8"
        ) as stream:
            stream.write("\nDefault Zig gates: make test\n")
        try:
            grade_coherence(temp_root)
        except EvalFailure:
            return
        raise EvalFailure("coherence negative control was not detected")


def grade_contract_negative_control() -> None:
    """Prove `grade_contract` can actually REJECT, per assertion mode.

    A grader that has never been shown to fail is indistinguishable from one
    that returns pass. `grade_coherence` has had a control since this suite
    landed; this is the same proof for the contract grader, exercising every
    arm that is supposed to refuse.
    """
    probe = "agents/methodology.md"
    if not (ROOT / probe).is_file():  # pragma: no cover - checkout invariant
        raise EvalFailure(f"contract negative control needs {probe}")

    def case(assertion_id: str, pattern: str, mode: str, paths: list[str]) -> dict[str, Any]:
        return {
            "id": "contract-negative-control",
            "contract_assertions": [
                {
                    "id": assertion_id,
                    "description": f"negative control: {assertion_id}",
                    "paths": paths,
                    "pattern": pattern,
                    "mode": mode,
                }
            ],
        }

    # Each of these MUST raise. `ZZZ` is chosen to be absent from any authored
    # surface; `\\bthe\\b` is chosen to be present in all of them.
    must_reject = [
        case("all-requires-every-path", "ZZZ_NEVER_PRESENT", "all", [probe]),
        case("any-requires-one-path", "ZZZ_NEVER_PRESENT", "any", [probe]),
        case("none-forbids-a-match", r"\bthe\b", "none", [probe]),
        case("missing-path-is-an-error", r"\bthe\b", "any", ["agents/does-not-exist.md"]),
    ]
    for probe_case in must_reject:
        assertion_id = probe_case["contract_assertions"][0]["id"]
        try:
            grade_contract(Path("<negative-control>"), probe_case)
        except EvalFailure:
            continue
        raise EvalFailure(
            f"contract grader negative control was not detected: {assertion_id}"
        )

    # ... and the positive arm, so the control proves DISCRIMINATION rather
    # than a grader that rejects everything it is handed.
    grade_contract(
        Path("<negative-control>"),
        case("any-accepts-a-real-match", r"\bthe\b", "any", [probe]),
    )


class SelfTestConstructionError(EvalFailure):
    """An assertion's seeded-violation self-test could not be built.

    D4 (decision, artifact 619): an assertion without a self-test fails the
    suite. An unconstructable mutation is therefore a suite failure, never a
    silently skipped self-test -- so this is a subclass of `EvalFailure`,
    not a distinct control-flow branch a caller could swallow.
    """


def named_selftest_failure(case_id: str, exc: SelfTestConstructionError) -> str:
    """Prefix a `SelfTestConstructionError`'s message with `case_id` (task
    6893). The error is raised where only the assertion id is in scope, so
    its raw message already starts with `"<assertion-id>: ..."`; this
    makes the combined message `"<case-id>/<assertion-id>: ..."` -- the
    same `label` shape `run_assertion_selftests` already uses for the
    `CaseOutcome` tuple -- without double-prefixing a message some other
    caller already combined.
    """
    message = str(exc)
    if message.startswith(f"{case_id}/"):
        return message
    return f"{case_id}/{message}"


def _split_top_level(pattern: str, sep: str) -> list[str]:
    """Split `pattern` on `sep` only where parenthesis depth is 0 and the
    separator is not escaped. Used to isolate top-level alternation
    (`a|b`) from alternation nested inside a group (`(a|b)`)."""
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    i = 0
    n = len(pattern)
    while i < n:
        ch = pattern[i]
        if ch == "\\" and i + 1 < n:
            current.append(pattern[i : i + 2])
            i += 2
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(current))
            current = []
            i += 1
            continue
        current.append(ch)
        i += 1
    parts.append("".join(current))
    return parts


def literal_from_pattern(pattern: str) -> str | None:
    """Derive one literal string that satisfies `pattern`, for seeding a
    must-not-match assertion's forbidden text (C3 / D4).

    Handles the shapes this suite's `none`-mode assertions actually use:
    plain text, an escaped literal metacharacter (`\\$`), a dropped
    zero-width anchor (`\\b`, `^`, `$`), and the first arm of an
    alternation (top-level `a|b`, or grouped `(a|b)`). Any other regex
    construct (character classes, quantifiers, unescaped wildcards) has no
    single deterministic literal, so this returns `None` rather than
    guessing -- the caller treats `None` as a suite failure, not a skip.
    """
    pattern = python_pattern(pattern)
    alternatives = _split_top_level(pattern, "|")
    if len(alternatives) > 1:
        return literal_from_pattern(alternatives[0])
    out: list[str] = []
    i = 0
    n = len(pattern)
    while i < n:
        ch = pattern[i]
        if ch == "\\":
            if i + 1 >= n:
                return None
            nxt = pattern[i + 1]
            if nxt == "b":
                i += 2
                continue
            if nxt in "sSdDwWnrt":
                return None
            out.append(nxt)
            i += 2
            continue
        if ch == "(":
            depth = 1
            j = i + 1
            while j < n and depth:
                if pattern[j] == "\\":
                    j += 2
                    continue
                if pattern[j] == "(":
                    depth += 1
                elif pattern[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if depth != 0:
                return None
            group = pattern[i + 1 : j]
            if group.startswith("?:"):
                group = group[2:]
            if j + 1 < n and pattern[j + 1] in "*?+{":
                return None
            literal = literal_from_pattern(group)
            if literal is None:
                return None
            out.append(literal)
            i = j + 1
            continue
        if ch in "^$":
            i += 1
            continue
        if ch in ".*+?[]{}|":
            return None
        out.append(ch)
        i += 1
    result = "".join(out)
    return result or None


def seed_selftest_mutation(temp_root: Path, assertion: dict[str, Any]) -> None:
    """Mutate the copies of `assertion["paths"]` under `temp_root` so this
    ONE assertion, evaluated in isolation, is guaranteed to fail: the
    matched text is deleted for a must-match assertion (mode `all`/`any`),
    or a literal satisfying the pattern is appended for a must-not-match
    assertion (mode `none`). Raises `SelfTestConstructionError` when no
    mutation can be built -- never returns having done nothing.
    """
    assertion_id = assertion["id"]
    mode = assertion["mode"]
    pattern = assertion["pattern"]
    flags = re.MULTILINE | (re.DOTALL if assertion.get("multiline", False) else 0)
    try:
        compiled = re.compile(python_pattern(pattern), flags)
    except re.error as exc:
        raise SelfTestConstructionError(
            f"{assertion_id}: invalid evaluation regex {pattern!r}: {exc}"
        ) from exc
    if mode in ("all", "any"):
        mutated = False
        for relative in assertion["paths"]:
            target = temp_root / relative
            text = target.read_text(encoding="utf-8")
            if compiled.search(text) is None:
                continue
            # Remove EVERY occurrence, not just the first: a path where the
            # pattern appears more than once still counts as matched after
            # deleting a single occurrence, which would leave the self-test
            # unable to falsify the assertion.
            target.write_text(compiled.sub("", text), encoding="utf-8")
            mutated = True
        if not mutated:
            raise SelfTestConstructionError(
                f"{assertion_id}: must-match pattern has no literal occurrence "
                "to delete in any declared path"
            )
        return
    if mode == "none":
        literal = literal_from_pattern(pattern)
        if literal is None:
            raise SelfTestConstructionError(
                f"{assertion_id}: forbidden pattern has no constructible literal "
                "to seed"
            )
        target = temp_root / assertion["paths"][0]
        with target.open("a", encoding="utf-8") as stream:
            stream.write(f"\n{literal}\n")
        return
    raise SelfTestConstructionError(f"{assertion_id}: unsupported assertion mode {mode!r}")


def run_one_assertion_selftest(case_id: str, assertion: dict[str, Any]) -> None:
    """Build a mutated copy of `assertion["paths"]`, seed the violation this
    assertion is supposed to catch, and require `grade_contract` to reject
    it. Raises `EvalFailure` (via `SelfTestConstructionError` or the
    re-raised assertion below) rather than returning a boolean, so a caller
    cannot silently ignore either "could not construct" or "did not fail".
    """
    temp_root = Path(tempfile.mkdtemp(prefix="planar-eval-selftest-"))
    try:
        for relative in assertion["paths"]:
            source = ROOT / relative
            if not source.is_file():
                raise SelfTestConstructionError(
                    f"{assertion['id']}: references missing path: {relative}"
                )
            target = temp_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        seed_selftest_mutation(temp_root, assertion)
        single_case = {"id": case_id, "contract_assertions": [assertion]}
        try:
            grade_contract(Path("<selftest>"), single_case, root=temp_root)
        except EvalFailure:
            return
        raise EvalFailure(
            f"{case_id}/{assertion['id']}: self-test did not fail under its "
            "seeded violation"
        )
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)


def run_assertion_selftests(
    cases: list[tuple[Path, dict[str, Any]]]
) -> tuple[int, int, list[CaseOutcome]]:
    """Run `run_one_assertion_selftest` for every contract assertion across
    `cases` (C3 / D4), without stopping at the first one that fails to
    falsify (C6 / task 6840). Returns `(self_tests_run, total_assertions,
    failures)`: `self_tests_run` counts only the assertions whose self-test
    passed, so a caller that still wants the old equality check can compare
    it against `total_assertions` -- but `failures` is authoritative and is
    empty exactly when the two counts match. An assertion added without a
    working self-test -- which, given generation is automatic here, can
    only mean this function's call site was bypassed -- shows up as a
    `failures` entry rather than a silent pass.
    """
    total_assertions = 0
    self_tests_run = 0
    failures: list[CaseOutcome] = []
    for _, case in cases:
        if "contract" not in case["tiers"]:
            continue
        for assertion in case["contract_assertions"]:
            total_assertions += 1
            case_id = case["id"]
            label = f"{case_id}/{assertion['id']}"
            try:
                run_one_assertion_selftest(case_id, assertion)
            except SelfTestConstructionError as exc:
                # Task 6893: `SelfTestConstructionError` is raised deep
                # inside `seed_selftest_mutation` / `run_one_assertion_selftest`,
                # which only ever see the assertion, so its own message
                # names the assertion id but not the case id. The same
                # assertion id can be reused across cases, so a message
                # with only the assertion id cannot tell an operator WHICH
                # case's self-test failed to construct. Wrap it here,
                # where both ids are in scope, rather than threading
                # case_id further down.
                failures.append((label, "fail", named_selftest_failure(case_id, exc)))
                continue
            except EvalFailure as exc:
                failures.append((label, "fail", str(exc)))
                continue
            self_tests_run += 1
    return self_tests_run, total_assertions, failures


def create_artifacts(
    case_id: str, label: str, results_dir: Path | None
) -> Path:
    if results_dir is not None:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        path = results_dir / case_id / f"{label}-{stamp}"
        path.mkdir(parents=True, exist_ok=False)
        return path
    return Path(tempfile.mkdtemp(prefix=f"planar-eval-{case_id}-"))


def finish_artifacts(path: Path, options: Options) -> None:
    if options.keep or options.results_dir is not None:
        print(f"artifacts: {path}", flush=True)
    else:
        shutil.rmtree(path)


def write_grade(
    artifact_dir: Path,
    status: str,
    case_id: str,
    options: Options,
    reason: str | None = None,
    *,
    target: Path | None = None,
    usage: dict[str, Any] | None = None,
) -> Path:
    """Write the grade record. Writes `artifact_dir/grade.json` by default;
    `target` (used by `regrade_artifacts`, C6 / task 6840) redirects the
    write to a different path so a regrade of retained artifacts never
    overwrites the original run's verdict.

    `usage` (task 6859) carries calls-per-task and provider token/cost
    counters extracted from the transcript; omitted for modes (contract,
    lifecycle-fixture) that never invoke a real vendor host.
    """
    path = target if target is not None else artifact_dir / "grade.json"
    record: dict[str, Any] = {
        "status": status,
        "case_id": case_id,
        "vendor": options.vendor,
        "surface": options.surface,
        "mode": options.mode,
        "reason": reason,
    }
    if usage is not None:
        record["usage"] = usage
    write_json(path, record)
    return path


def live_failure(
    artifact_dir: Path, case_id: str, options: Options, reason: str
) -> EvalFailure:
    write_grade(artifact_dir, "fail", case_id, options, reason)
    return EvalFailure(f"{case_id}: {reason}", artifact_dir)


def logical_sqlite_dump(path: Path) -> str:
    connection = sqlite3.connect(path, timeout=30)
    try:
        return "\n".join(connection.iterdump()) + "\n"
    finally:
        connection.close()


def normalized_dispatch_state(value: dict[str, Any]) -> dict[str, Any]:
    available = [
        {
            "id": item.get("id"),
            "slug": item.get("slug"),
            "status": item.get("status"),
            "next_action": item.get("next_action"),
        }
        for item in value.get("available", [])
    ]
    return {
        "plan_id": value.get("plan_id"),
        "available": available,
        "claimed": value.get("claimed", []),
        "stale": value.get("stale", []),
        "blocked": value.get("blocked", []),
        "summary": value.get("summary", {}),
    }


def git_state(repo: Path, env: dict[str, str]) -> str:
    status = run_command(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=repo,
        env=env,
    ).stdout
    refs = run_command(
        ["git", "for-each-ref", "--format=%(refname) %(objectname)"],
        cwd=repo,
        env=env,
    ).stdout
    return status + refs


def require_current_installed_projection() -> None:
    require_commands(["scriptorium"])
    run_passthrough([str(ROOT / "scripts" / "check-self-installed.sh")], cwd=ROOT)


def collect_live_after(
    artifact_dir: Path, repo: Path, env: dict[str, str], plan_id: str
) -> None:
    after = run_json(
        ["planar", "plan", "next", plan_id, "--json"], cwd=repo, env=env
    )
    write_json(artifact_dir / "after.json", after)
    write_json(
        artifact_dir / "after.dispatch-state.json", normalized_dispatch_state(after)
    )
    write_text(
        artifact_dir / "after.planar.sql",
        logical_sqlite_dump(Path(env["PLANAR_DB"])),
    )
    write_text(artifact_dir / "after.git-state", git_state(repo, env))
    claims = run_json(
        ["planar-watch", "ps", "--plan", plan_id, "--json"], cwd=repo, env=env
    )
    write_json(artifact_dir / "claims.after.json", claims)


def explicit_work_type_assignment(text: str, work_type: str) -> bool:
    escaped = re.escape(work_type)
    pattern = (
        rf"work[_ -]?type[^A-Za-z0-9]{{0,12}}{escaped}"
        # Table cell only. `/` used to be accepted as a delimiter here, which
        # also matched slash-separated PROSE enumerations -- "no citable
        # unresolved schema/architectural/engine decision" is a statement that
        # the work type does NOT apply, and it was being graded as an
        # assignment. Pipes are the preview's actual column separator.
        rf"|[|][\s`]*{escaped}[\s`]*[|]"
        rf"|\(\s*{escaped}\s*\)"
    )
    return re.search(pattern, text, re.IGNORECASE) is not None


class UnsafeModelValue(ValueError):
    """A host/candidate value that must not reach a command line."""


def validate_model_value(field: str, value: object) -> str:
    """Return `value` if it is safe to pass as a single argv element.

    Model ids are opaque operator data: they are passed as their own argv
    element and never interpolated into a shell string, so punctuation like
    `;` or `$(...)` is content rather than syntax. What must be refused is
    what would corrupt the record or split one argument into two — control
    characters and whitespace — plus the empty string, which would silently
    become "no model" rather than an error.
    """
    if not isinstance(value, str):
        raise UnsafeModelValue(f"{field} must be a string, got {type(value).__name__}")
    if not value:
        raise UnsafeModelValue(f"{field} must not be empty")
    for ch in value:
        if ord(ch) < 0x20 or ord(ch) == 0x7F:
            raise UnsafeModelValue(f"{field} contains a control character")
    if any(ch.isspace() for ch in value):
        # A value with whitespace is ambiguous: it would be one argv element
        # here but read as several by anything that re-splits it.
        raise UnsafeModelValue(f"{field} must be a single argument (no whitespace)")
    return value


def resolve_model_inputs(live: dict[str, Any]) -> tuple[str | None, str | None]:
    """Validate the two INDEPENDENT model inputs on a live case.

    `host_model` is what the orchestrator process runs as; `delegated_candidate`
    is what it dispatches a child role to. They are deliberately not derived
    from one another — collapsing them into one field would make a cross-host
    leak invisible, because a single field cannot disagree with itself.
    """
    host = live.get("host_model")
    delegated = live.get("delegated_candidate")
    return (
        validate_model_value("host_model", host) if host is not None else None,
        validate_model_value("delegated_candidate", delegated) if delegated is not None else None,
    )


# Alias: canonical definition moved to `vendors.py` (task 6849 split).
model_argv = vendors.model_argv


def cross_host_assignment(text: str, vendor: str) -> bool:
    if vendor == "codex":
        pattern = r"model:\s*`?claude-|[|]\s*`?claude-[^|]*[|]"
    else:
        pattern = r"model:\s*`?gpt-5\.|[|]\s*`?gpt-5\.[^|]*[|]"
    return re.search(pattern, text, re.IGNORECASE) is not None


def grade_live_artifacts(
    case: dict[str, Any], artifact_dir: Path, options: Options
) -> None:
    case_id = case["id"]
    required = [
        "before.dispatch-state.json",
        "after.dispatch-state.json",
        "before.planar.sql",
        "after.planar.sql",
        "before.git-state",
        "after.git-state",
        "claims.after.json",
        "final.txt",
        "transcript.txt",
        "transcript.jsonl",
    ]
    missing = [name for name in required if not (artifact_dir / name).is_file()]
    if missing:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"artifact set is incomplete: {', '.join(missing)}",
        )
    comparisons = [
        ("before.dispatch-state.json", "after.dispatch-state.json", "dispatch state"),
        ("before.planar.sql", "after.planar.sql", "Planar state"),
        ("before.git-state", "after.git-state", "Git or worktree files"),
    ]
    for before_name, after_name, label in comparisons:
        before = (artifact_dir / before_name).read_bytes()
        after = (artifact_dir / after_name).read_bytes()
        if before != after:
            raise live_failure(
                artifact_dir, case_id, options, f"{label} changed before approval"
            )
    claims = read_json(artifact_dir / "claims.after.json")
    if len(claims.get("active", [])) != 0:
        raise live_failure(
            artifact_dir, case_id, options, "claim created before approval"
        )

    final = (artifact_dir / "final.txt").read_text(encoding="utf-8")
    transcript = (artifact_dir / "transcript.txt").read_text(
        encoding="utf-8", errors="replace"
    )
    raw = (artifact_dir / "transcript.jsonl").read_text(
        encoding="utf-8", errors="replace"
    )
    for task in case["tasks"]:
        slug = task["slug"]
        rows = [line for line in final.splitlines() if slug in line]
        if not rows:
            raise live_failure(
                artifact_dir, case_id, options, f"preview omitted task slug: {slug}"
            )
        task_row = "\n".join(rows)
        if options.vendor == "codex" and re.search(
            r"claude-(haiku|sonnet|opus|fable)", task_row
        ):
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"task row {slug} proposed a Claude model on Codex",
            )
        if options.vendor == "claude" and re.search(r"gpt-5\.", task_row):
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"task row {slug} proposed a Codex model on Claude",
            )
    expected = case.get("expected", {})
    for term in expected.get("required_preview_terms", []):
        if re.search(term, final, re.IGNORECASE) is None:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"preview omitted required term: {term}",
            )
    expected_tier = expected.get("tier")
    if expected_tier and re.search(
        re.escape(str(expected_tier)), final, re.IGNORECASE
    ) is None:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"preview did not propose {expected_tier} tier",
        )
    forbidden = expected.get("forbidden_work_type")
    if forbidden and explicit_work_type_assignment(final, str(forbidden)):
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"preview assigned forbidden work type: {forbidden}",
        )
    if expected.get("requires_explicit_gate") and re.search(
        r"confirm|approval|accept|wait", final, re.IGNORECASE
    ) is None:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            "preview did not stop at an explicit gate",
        )
    if "Full-history forked agents inherit the parent agent type" in transcript:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            "Codex direct-agent spawn used an incompatible full-history fork",
        )
    if re.search(r"UnknownSubcommand|No such file or directory", transcript):
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            "host trace contains an invalid command or missing reference",
        )
    if (
        options.vendor == "codex"
        and options.surface == "agent"
        and "orchestrator/SKILL.md" in raw
    ):
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            "Codex direct-agent adapter leaked the skill into the parent context",
        )
    expected_model = case["live"]["expected_models"][options.vendor]
    if expected_model not in final:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"preview did not bind expected model: {expected_model}",
        )
    if cross_host_assignment(final, options.vendor):
        other = "Claude" if options.vendor == "codex" else "Codex"
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"preview assigned a {other} model on {options.vendor.title()}",
        )


def prepare_phase3_preview(
    case_path: Path, case: dict[str, Any], options: Options
) -> Path:
    """PREPARE step (task 6849 split): build the arena, seed the Planar
    plan/tasks, snapshot the `before` state, and write `run.json` --
    everything the live phase3-preview case needs before a vendor host is
    ever invoked. Never starts `claude`/`codex`. Returns the artifact dir;
    the RUN step (`run_phase3_preview_from_prepared`) resumes from it.
    """
    case_id = case["id"]
    artifact_dir = create_artifacts(
        case_id, f"{options.vendor}-{options.surface}", options.results_dir
    )
    repo = artifact_dir / "repo"
    repo.mkdir()
    arena_root = artifact_dir / "arena"
    env = arena.make_arena(arena_root)
    arena.assert_isolated(env, arena_root)
    # Stage the running vendor's read surfaces (slash commands, skills,
    # agents, auth) from the real install into the scratch arena before
    # anything runs (Planar question 983): the live prompt below is
    # `/pl-orchestrator <plan-id>` / a codex `$orchestrator` invocation,
    # which resolves from the vendor's real config dir, not an empty
    # scratch one. Fails closed (VendorStagingError) before `git init` or
    # any host process if a required surface is missing. Threads
    # options.surface through so `agents` is promoted to required when
    # this run invokes the vendor as an agent (surface == "agent").
    arena.stage_vendor_config(env, options.vendor, surface=options.surface)
    # Staging a vendor's read surfaces is not the same as authenticating
    # it: Keychain-backed `claude` login does not follow into a scratch
    # `CLAUDE_CONFIG_DIR` at all (Planar artifact 626 / task 6872). Fails
    # closed (VendorAuthError) before any host process starts, naming
    # exactly which env var or staged file is missing — never a token
    # value.
    arena.assert_vendor_auth(env, options.vendor)
    try:
        run_command(["git", "init", "-q"], cwd=repo, env=env)
        opposite = "claude" if options.vendor == "codex" else "codex"
        write_text(Path(env["PLANAR_CONFIG_PATH"]), f'[defaults]\nvendor = "{opposite}"\n')
        run_command(["planar", "init", "--name", "orchestrator-eval", "--json"], cwd=repo, env=env)
        plan = run_json(
            [
                "planar",
                "plan",
                "create",
                case_id,
                "--scope",
                "global",
                "--status",
                "active",
                "--json",
            ],
            cwd=repo,
            env=env,
        )
        plan_id = str(plan["id"])
        for task in case["tasks"]:
            run_command(
                [
                    "planar",
                    "task",
                    "add",
                    task["title"],
                    "--scope",
                    "global",
                    "--plan",
                    plan_id,
                    "--slug",
                    task["slug"],
                    "--editor=false",
                    "--json",
                ],
                cwd=repo,
                env=env,
            )
        before = run_json(
            ["planar", "plan", "next", plan_id, "--json"], cwd=repo, env=env
        )
        write_json(artifact_dir / "before.json", before)
        write_json(
            artifact_dir / "before.dispatch-state.json",
            normalized_dispatch_state(before),
        )
        write_text(
            artifact_dir / "before.planar.sql",
            logical_sqlite_dump(Path(env["PLANAR_DB"])),
        )
        write_text(artifact_dir / "before.git-state", git_state(repo, env))
        # Task 6850: verify the per-case tool allowlist at PREPARE time,
        # before any vendor host is invoked, so a typo in `allowed_tools`
        # fails here rather than mid-run inside a transcript.
        try:
            vendors.validate_allowed_tools(resolve_allowed_tools(case["live"]))
        except vendors.UnknownToolError as exc:
            raise EvalFailure(
                f"{case_id}: live allowed_tools names an unknown tool: {exc.tool}",
                artifact_dir,
            )
        write_json(
            artifact_dir / "run.json",
            {
                "mode": "live",
                "case_id": case_id,
                "case_path": str(case_path.relative_to(ROOT)),
                "vendor": options.vendor,
                "surface": options.surface,
                "plan_id": plan_id,
                "repo": "repo",
            },
        )
        return artifact_dir
    except (EvalFailure, EvalBlocked) as exc:
        if exc.artifacts is None:
            exc.artifacts = artifact_dir
        raise


def run_phase3_preview_from_prepared(
    case_path: Path, case: dict[str, Any], options: Options, artifact_dir: Path
) -> None:
    """RUN + GRADE steps (task 6849 split): invoke the vendor host against
    an artifact dir `prepare_phase3_preview` already prepared, then grade.
    Recomputes the prompt/model/timeout deterministically from `case` and
    `run.json`'s `plan_id` rather than threading them through as
    arguments, matching the pattern `regrade_artifacts` already uses to
    resume grading from a bare artifact dir.
    """
    case_id = case["id"]
    repo = artifact_dir / "repo"
    arena_root = artifact_dir / "arena"
    env = arena.make_arena(arena_root)
    arena.assert_isolated(env, arena_root)
    # Re-checked here (not just at prepare time): a `--prepare` and a later
    # separate `--run` can be two different process invocations, and auth is
    # only ever read from the live process environment (never persisted into
    # the scratch arena tree), so it must be verified again immediately
    # before this step's own host process starts.
    arena.assert_vendor_auth(env, options.vendor)
    run_meta = read_json(artifact_dir / "run.json")
    plan_id = run_meta["plan_id"]
    prompt = case["live"]["prompt"].replace("{{PLAN_ID}}", plan_id)
    host_model, _delegated_candidate = resolve_model_inputs(case["live"])
    allowed_tools = resolve_allowed_tools(case["live"])
    raw = artifact_dir / "transcript.jsonl"
    timeout = int(case["live"].get("timeout_seconds", 300))
    try:
        print(
            f"RUN: {case_id} ({options.vendor}/{options.surface}), timeout {timeout}s",
            flush=True,
        )
        print(f"transcript: {raw}", flush=True)
        print(f"follow: tail -f {raw}", flush=True)

        command = vendors.build_preview_command(
            vendor=options.vendor,
            surface=options.surface,
            plan_id=plan_id,
            prompt=prompt,
            host_model=host_model,
            allowed_tools=allowed_tools,
        )
        rc = run_to_file(command, raw, cwd=repo, env=env, timeout_seconds=timeout)
        raw_text = raw.read_text(encoding="utf-8", errors="replace")
        limited = rate_limit_reason(raw_text)
        if limited:
            write_grade(artifact_dir, "blocked", case_id, options, limited)
            raise EvalBlocked(
                f"{case_id}: {options.vendor}/{options.surface} {limited}",
                artifact_dir,
            )
        if rc == 124:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"{options.vendor}/{options.surface} live run timed out after {timeout}s",
            )
        if rc != 0:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"{options.vendor}/{options.surface} invocation exited {rc}",
            )
        usage = extract_usage(raw, options.vendor)
        case_budget_usd = case["live"].get("budget", {}).get("max_usd")
        enforce_spend_ceiling(
            artifact_dir=artifact_dir,
            case_id=case_id,
            options=options,
            repo=repo,
            env=env,
            plan_id=plan_id,
            usage=usage,
            case_budget_usd=case_budget_usd,
        )
        extract_host_transcript(raw, options.vendor, artifact_dir)
        collect_live_after(artifact_dir, repo, env, plan_id)
        grade_live_artifacts(case, artifact_dir, options)
        write_grade(artifact_dir, "pass", case_id, options, usage=usage)
        pass_line(
            f"{case_id}: {options.vendor}/{options.surface} live phase3-preview"
        )
        finish_artifacts(artifact_dir, options)
    except (EvalFailure, EvalBlocked) as exc:
        if exc.artifacts is None:
            exc.artifacts = artifact_dir
        raise


def run_phase3_preview(
    case_path: Path, case: dict[str, Any], options: Options
) -> None:
    """Single-step live phase3-preview run: prepare, then run+grade."""
    artifact_dir = prepare_phase3_preview(case_path, case, options)
    run_phase3_preview_from_prepared(case_path, case, options, artifact_dir)


def toml_string(value: str) -> str:
    return json.dumps(value)


def write_codex_agent(
    repo: Path, name: str, description: str, instructions_path: Path
) -> None:
    instructions = instructions_path.read_text(encoding="utf-8")
    write_text(
        repo / ".codex" / "agents" / f"{name}.toml",
        f"name = {toml_string(name)}\n"
        f"description = {toml_string(description)}\n"
        f"developer_instructions = {toml_string(instructions)}\n",
    )


def write_claude_agent(
    repo: Path, name: str, description: str, instructions_path: Path
) -> None:
    instructions = instructions_path.read_text(encoding="utf-8")
    write_text(
        repo / ".claude" / "agents" / f"{name}.md",
        f"---\nname: {json.dumps(name)}\n"
        f"description: {json.dumps(description)}\n---\n\n{instructions}\n",
    )


def prepare_lifecycle_fixture(
    case_path: Path, case: dict[str, Any], options: Options, label: str
) -> LifecycleContext:
    case_id = case["id"]
    fixture_root = FIXTURES_DIR / case["setup"]["fixture"]
    if not (fixture_root / "repo").is_dir():
        raise EvalFailure(f"{case_id} references missing fixture: {fixture_root}")
    artifacts = create_artifacts(case_id, label, options.results_dir)
    repo = artifacts / "repo"
    shutil.copytree(fixture_root / "repo", repo)
    arena_root = artifacts / "arena"
    env = arena.make_arena(arena_root)
    arena.assert_isolated(env, arena_root)
    # Only the live-host lifecycle mode spawns a real vendor CLI; the
    # fixture-replay mode (options.vendor == "") drives `.eval/control.sh`
    # instead and never needs the real vendor config, so it must not be
    # made to fail closed over a surface (e.g. codex `auth.json`) it never
    # reads.
    if options.vendor:
        arena.stage_vendor_config(env, options.vendor, surface=options.surface)
        # Same fail-closed auth requirement as run_phase3_preview: staging
        # is not authenticating (Planar artifact 626 / task 6872). Skipped
        # alongside staging for fixture replay (options.vendor == ""),
        # which never spawns a real vendor host.
        arena.assert_vendor_auth(env, options.vendor)
    try:
        run_command(["git", "init", "-q"], cwd=repo, env=env)
        run_command(["git", "config", "user.name", "Planar Eval"], cwd=repo, env=env)
        run_command(
            ["git", "config", "user.email", "planar-eval@example.invalid"],
            cwd=repo,
            env=env,
        )
        run_command(["git", "add", "."], cwd=repo, env=env)
        run_command(
            ["git", "commit", "-q", "-m", "eval: controlled lifecycle baseline"],
            cwd=repo,
            env=env,
        )
        write_text(
            Path(env["PLANAR_CONFIG_PATH"]),
            f'[defaults]\nvendor = "{options.vendor or "codex"}"\n',
        )
        run_command(["planar", "init", "--name", case_id, "--json"], cwd=repo, env=env)
        plan = run_json(
            [
                "planar",
                "plan",
                "create",
                case_id,
                "--scope",
                "global",
                "--status",
                "active",
                "--json",
            ],
            cwd=repo,
            env=env,
        )
        plan_id = str(plan["id"])
        task = case["tasks"][0]
        next_action = (
            "Run the controlled coder against src/value.txt and observe make test passing."
        )
        task_value = run_json(
            [
                "planar",
                "task",
                "add",
                task["title"],
                "--scope",
                "global",
                "--plan",
                plan_id,
                "--slug",
                task["slug"],
                "--body",
                "Acceptance: src/value.txt contains approved after reviewer approval. "
                "See docs/tech-spec.md.",
                "--next-action",
                next_action,
                "--editor=false",
                "--json",
            ],
            cwd=repo,
            env=env,
        )
        task_id = str(task_value["id"])
        run_command(
            [
                "planar",
                "capture",
                "snapshot",
                "--task",
                task_id,
                "--next-action",
                next_action,
                "--note",
                "orchestration_checkpoint: v1\n"
                "stage: verified-slice\n"
                "iteration_scope: none\n"
                "iteration: 0\n"
                "result: lifecycle-fixture-ready",
                "--json",
            ],
            cwd=repo,
            env=env,
        )

        for directory in (
            repo / ".eval" / "bin",
            repo / ".codex" / "agents",
            repo / ".claude" / "agents",
        ):
            directory.mkdir(parents=True, exist_ok=True)
        write_text(repo / ".eval" / "events.jsonl", "")
        write_text(
            repo / ".eval" / "scenario",
            case["setup"]["specialist_scenario"] + "\n",
        )
        real_planar_agent = shutil.which("planar-agent", path=env["PATH"])
        if real_planar_agent is None:
            raise EvalFailure("missing command: planar-agent")
        write_text(repo / ".eval" / "real-planar-agent", real_planar_agent + "\n")
        shutil.copy2(fixture_root / "control.sh", repo / ".eval" / "control.sh")
        shutil.copy2(
            fixture_root / "planar-agent", repo / ".eval" / "bin" / "planar-agent"
        )
        shutil.copy2(
            fixture_root / "record_observed.py",
            repo / ".eval" / "record_observed.py",
        )
        os.chmod(repo / ".eval" / "control.sh", 0o755)
        os.chmod(repo / ".eval" / "bin" / "planar-agent", 0o755)
        os.chmod(repo / ".eval" / "record_observed.py", 0o755)
        instructions = (
            "Lifecycle-eval boundary: execute this orchestrator contract directly.\n\n"
            "Do not read or invoke any installed skill. The project-scoped controlled "
            "coder, reviewer, and test-coder definitions are the only specialist "
            "contracts for this run.\n\n"
            "Never edit source content in the orchestrator context.\n\n"
            + (ROOT / "agents" / "orchestrator.md").read_text(encoding="utf-8")
        )
        instructions_path = repo / ".eval" / "orchestrator.instructions.md"
        write_text(instructions_path, instructions)
        write_text(
            repo / ".codex" / "config.toml",
            "[agents]\nmax_concurrent_threads_per_session = 4\n",
        )
        role_data = [
            (
                "orchestrator",
                "Runs the Planar orchestration lifecycle without authoring source changes.",
                instructions_path,
            ),
            (
                "coder",
                "Controlled lifecycle-eval coder.",
                fixture_root / "coder.instructions.md",
            ),
            (
                "reviewer",
                "Controlled lifecycle-eval reviewer.",
                fixture_root / "reviewer.instructions.md",
            ),
            (
                "test-coder",
                "Controlled lifecycle-eval test-coder.",
                fixture_root / "test-coder.instructions.md",
            ),
        ]
        for name, description, source in role_data:
            write_codex_agent(repo, name, description, source)
            write_claude_agent(repo, name, description, source)
        for support in ("cross-scope-writes", "doctrine", "methodology", "models"):
            for vendor in ("codex", "claude"):
                shutil.copy2(
                    ROOT / "agents" / f"{support}.md",
                    repo / f".{vendor}" / "agents" / f"{support}.md",
                )
        env["PATH"] = str(repo / ".eval" / "bin") + os.pathsep + env["PATH"]
        before = run_json(
            ["planar", "plan", "next", plan_id, "--json"], cwd=repo, env=env
        )
        write_json(artifacts / "plan.before.json", before)
        write_text(
            artifacts / "git-status.before.txt",
            run_command(["git", "status", "--short"], cwd=repo, env=env).stdout,
        )
        # Task 6850: verify the per-case tool allowlist at PREPARE time,
        # before any vendor host is invoked, so a typo in `allowed_tools`
        # fails here rather than mid-run inside a transcript. Skipped for
        # fixture-replay (options.vendor == ""), which never spawns a real
        # vendor host and so never reads this allowlist.
        if options.vendor:
            try:
                vendors.validate_allowed_tools(
                    resolve_allowed_tools(case["lifecycle"])
                )
            except vendors.UnknownToolError as exc:
                raise EvalFailure(
                    f"{case_id}: lifecycle allowed_tools names an unknown tool: "
                    f"{exc.tool}",
                    artifacts,
                )
        write_json(
            artifacts / "run.json",
            {
                "mode": options.mode,
                "case_id": case_id,
                "case_path": str(case_path.relative_to(ROOT)),
                "vendor": options.vendor,
                "surface": options.surface,
                "plan_id": plan_id,
                "task_id": task_id,
                "repo": "repo",
                # C6: marks this artifact set as one that MUST carry a
                # `repo/.eval/observed` tree by the time grading runs. A
                # run.json at or above ARTIFACT_FORMAT_OBSERVED_LOG (task
                # 6876) makes a missing observed/ dir a hard grading
                # failure rather than a silent legacy skip -- see
                # grade_lifecycle_artifacts.
                "artifact_format": ARTIFACT_FORMAT_OBSERVED_LOG,
            },
        )
        return LifecycleContext(
            case_path, case, artifacts, repo, env, plan_id, task_id
        )
    except Exception as exc:
        if isinstance(exc, (EvalFailure, EvalBlocked)) and exc.artifacts is None:
            exc.artifacts = artifacts
        raise


def event_objects(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    if not path.exists():
        return events
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            value = json.loads(line)
            if isinstance(value, dict):
                events.append(value)
    return events


# `run.json["artifact_format"]` at or above this value promises a
# `repo/.eval/observed` tree exists by grading time (task 6876). A retained
# artifact set with no `artifact_format` key (or a value below this) predates
# C2's observed log and is graded under the legacy no-observed-log path; one
# at or above this value with `observed/` missing is a real defect (the
# collection run failed to produce it) and grading must fail, not skip.
ARTIFACT_FORMAT_OBSERVED_LOG = 2

# The planar-agent wrapper's argv[0] -> legacy lifecycle-event-name mapping.
# Only a forwarded SUCCESS produces a named lifecycle event: a failed
# terminal verb (e.g. a second `complete` on an already-completed claim) is
# retained in the observed record for the terminal-verb assertions in
# `assert_terminal_verb_lifecycle`, but does not itself advance the ordered
# lifecycle the grader checks.
OBSERVED_EVENT_NAMES = {
    "pull": "claim-acquired",
    "claim": "claim-acquired",
    "complete": "task-completed",
    "fail": "task-failed",
    "release": "claim-released",
    "block": "task-blocked",
}
TERMINAL_VERBS = {"complete", "fail", "release", "block"}
CLAIM_VERBS = {"pull", "claim"}
# C5: `release` is a hand-back the ritual explicitly allows to be followed
# by a re-claim (see lapsed-claim recovery: re-claim with
# --no-transition then complete). Only complete|fail|block gate "no
# claim/pull observed after a terminal verb on the same task" -- a claim
# observed after a `release` on the same task is the RECOVERY PATH, not a
# violation.
CLAIM_GATE_TERMINAL_VERBS = TERMINAL_VERBS - {"release"}

# The controlled-classic fixture's own `log_event` calls
# (fixtures/controlled-classic/control.sh) -- events NOT derived from a
# wrapped `planar-agent` verb. Kept as an explicit manifest, checked against
# the script itself by `test_fixture_log_events_matches_control_script`
# (task 6836), rather than re-parsing the shell script at import time.
FIXTURE_LOG_EVENTS = {
    "coder-finished",
    "review-request-changes",
    "review-approved",
    "test-coder-no-expansion",
    "task-completed-before-review",
}

# Every lifecycle event name a controlled fixture run can actually produce:
# either a successful wrapped verb (OBSERVED_EVENT_NAMES) or a `log_event`
# call in the specialist script (FIXTURE_LOG_EVENTS). `validate_case`
# rejects a lifecycle case whose `forbidden_events` names anything outside
# this set -- an event that can never fire makes the negative assertion
# unfalsifiable (task 6836, D4).
EMITTABLE_EVENTS = set(OBSERVED_EVENT_NAMES.values()) | FIXTURE_LOG_EVENTS


def observed_event_name(record: dict[str, Any]) -> str | None:
    argv = record.get("argv") or []
    if not argv or record.get("exit_code") != 0:
        return None
    command = argv[0]
    if command == "pull":
        # docs/cli-reference.md § JSON shapes: `pull` -> {ok, no_work,
        # claim_token?, claim?, task?, action_id?}. `pull` returns exit 0
        # with {ok:true, no_work:true} when nothing is eligible
        # (src/cmd/planar-agent/handlers/claims.cpp:194-196) -- that is NOT
        # a claim, and naming it `claim-acquired` would hand a polling
        # orchestrator a spurious event, possibly ordered after
        # `task-completed` (exactly the synthesized-order problem D3
        # deletes the audit fallback over).
        stdout = record.get("stdout")
        no_work = isinstance(stdout, dict) and bool(stdout.get("no_work"))
        has_token = isinstance(stdout, dict) and bool(stdout.get("claim_token"))
        if no_work and not has_token:
            return None
        return "claim-acquired"
    return OBSERVED_EVENT_NAMES.get(command)


def read_observed_records(observed_dir: Path) -> list[dict[str, Any]]:
    """Read every JSON document under a wrapper `observed/` directory,
    ordered by (ts, pid, seq) -- the file-per-call layout C2 requires so
    that concurrent wrapper lanes cannot interleave a record. A leftover
    `.tmp-observed-*` file from an interrupted write is ignored; the
    wrapper only ever renames a fully-written file into place.

    `seq` is always 0 now (task 6877 deleted the wrapper's cross-process
    lock/counter): each wrapper process records exactly once, and
    uniqueness of the on-disk filename comes from `record_observed.py`'s
    `<ts>-<pid>-<seq>.json` naming plus `os.replace`, not from `seq`
    itself. `pid` is the tie-breaker for two records sharing the exact
    same `time.time()` value -- it is always distinct across concurrently
    running wrapper processes, whereas `seq` no longer varies at all.
    """
    if not observed_dir.is_dir():
        return []
    records: list[dict[str, Any]] = []
    for path in sorted(observed_dir.glob("*.json")):
        if path.name.startswith(".tmp-"):
            continue
        value = read_json(path)
        if isinstance(value, dict):
            records.append(value)
    records.sort(key=observed_sort_key)
    return records


def observed_sort_key(record: dict[str, Any]) -> tuple[float, int, int]:
    """The (ts, pid, seq) ordering key shared by every place this module
    derives lifecycle event order from the observed log (task 6877)."""
    return (
        float(record.get("ts", 0)),
        int(record.get("pid", 0)),
        int(record.get("seq", 0)),
    )


def claim_token_of(argv: list[str]) -> str | None:
    for index, arg in enumerate(argv):
        if arg == "--claim" and index + 1 < len(argv):
            return argv[index + 1]
        if arg.startswith("--claim="):
            return arg.split("=", 1)[1]
    return None


def task_id_of(argv: list[str]) -> str | None:
    for index, arg in enumerate(argv):
        value = None
        if arg == "--entity" and index + 1 < len(argv):
            value = argv[index + 1]
        elif arg.startswith("--entity="):
            value = arg.split("=", 1)[1]
        if value and value.startswith("task:"):
            return value[len("task:") :]
    return None


def observed_stdout_field(record: dict[str, Any], name: str) -> Any:
    stdout = record.get("stdout")
    if isinstance(stdout, dict):
        return stdout.get(name)
    return None


def observed_task_id(record: dict[str, Any]) -> str | None:
    """The task id a wrapper-observed record targets.

    Per docs/cli-reference.md § JSON shapes, the real `planar-agent --json`
    output never carries a flat `task_id`: it is nested at `task.id`
    (`Task` matches `planar task show --json`), with `claim.entity_id` as
    the fallback on a verb whose shape has no `task` key (`claim` /
    `heartbeat` -> {ok, claim_token, claim}). Only `pull`'s argv can ever
    carry `--entity task:<id>` in this fixture's flows (`claim`'s task id
    comes from its own `--entity` argv the caller already used to build
    the claim), so the argv fallback is checked last, after both stdout
    shapes.
    """
    stdout = record.get("stdout")
    if isinstance(stdout, dict):
        task = stdout.get("task")
        if isinstance(task, dict) and task.get("id") is not None:
            return str(task["id"])
        claim = stdout.get("claim")
        if isinstance(claim, dict) and claim.get("entity_id") is not None:
            return str(claim["entity_id"])
    return task_id_of(record.get("argv") or [])


def assert_no_recording_failures(
    observed_dir: Path,
    *,
    artifacts: Path | None = None,
    case_id: str | None = None,
    options: Options | None = None,
) -> None:
    """C2: recording is fail-open at the wrapper (a `record_observed.py`
    failure warns to stderr and still forwards the real `$RC`, so the
    invocation itself succeeds from the caller's point of view). A dropped
    record is indistinguishable from a partial bypass unless something
    checks for it, so the wrapper writes a `.record-failed` sentinel on
    every recording failure; this treats that sentinel's mere presence as
    a failed run.
    """
    sentinel = observed_dir / ".record-failed"
    if not sentinel.is_file():
        return
    detail = sentinel.read_text(encoding="utf-8", errors="replace").strip()
    reason = f"wrapper-bypassed: observed log recording failed: {detail[:500]}"
    if artifacts is not None and case_id is not None and options is not None:
        raise live_failure(artifacts, case_id, options, reason)
    raise EvalFailure(reason)


def write_lifecycle_warnings(
    artifacts: Path, terminal_attempt_failures: list[dict[str, Any]]
) -> None:
    """C4: a FAILED terminal-verb attempt (e.g. a second `complete` the
    real binary rejected) does not fail the run and is excluded from the
    "exactly one terminal verb" count, but it is the same shape a genuine
    split-terminal defect would produce, so it is surfaced as a warning
    for a human/reviewer to read rather than silently dropped.
    """
    write_json(
        artifacts / "warnings.json",
        {"terminal_attempt_failures": terminal_attempt_failures},
    )


def assert_wrapper_not_bypassed(
    observed_records: list[dict[str, Any]],
    claims: list[dict[str, Any]],
    *,
    task_id: str | None = None,
    artifacts: Path | None = None,
    case_id: str | None = None,
    options: Options | None = None,
) -> None:
    """Decision D3: the audit-table order reconstruction is deleted
    outright rather than fixed. A run whose observed log is empty while
    the Planar audit shows a claim means the wrapper was bypassed -- real
    agent work happened without going through the PATH shim that produces
    the observed log -- and that is a failed run, not a gap to paper over
    with a synthesized `claim-acquired`/`task-completed` pair.

    C3: a FULLY empty observed log is the easy case. A wrapper can also be
    PARTIALLY bypassed -- some claims went through the wrapper, one did
    not (a stray direct `planar-agent claim` call, or a lane that dropped
    its wrapper env) -- and that leaves the observed log non-empty, so the
    plain emptiness check above misses it. Compare the count of
    successful, non-`no_work` claim-acquired records (see
    `observed_event_name`'s pull/no_work handling) against the audit's own
    claim count; any mismatch is a bypass, partial or otherwise.

    C7: `claims` (from `planar audit trail --kind task <task_id>`) is
    already task-scoped, but the observed log is the WHOLE run's log --
    it also carries the orchestrator's own `claim --entity plan:<id>` (a
    real call every orchestrator run makes) and any other task's pull/
    claim. Counting every `claim-acquired` record regardless of which
    task it targets over-counts against the task-scoped audit and reports
    a false `wrapper-bypassed: partial` on a perfectly clean run. When
    `task_id` is given, only records whose `observed_task_id(record)`
    matches count; a record this fixture can't attribute to a task at all
    (e.g. a plan-level claim) is excluded rather than guessed at.
    """

    def fail(reason: str) -> None:
        if artifacts is not None and case_id is not None and options is not None:
            raise live_failure(artifacts, case_id, options, reason)
        raise EvalFailure(reason)

    if not observed_records and claims:
        fail(
            "wrapper-bypassed: observed log is empty but the audit shows "
            "agent activity"
        )
    if not claims:
        return
    observed_claim_count = sum(
        1
        for record in observed_records
        if observed_event_name(record) == "claim-acquired"
        and (task_id is None or observed_task_id(record) == task_id)
    )
    if observed_claim_count != len(claims):
        fail(
            "wrapper-bypassed: partial -- observed log shows "
            f"{observed_claim_count} claim-acquired record(s) but the "
            f"audit shows {len(claims)} claim(s)"
        )


def assert_terminal_verb_lifecycle(
    observed_records: list[dict[str, Any]],
    *,
    artifacts: Path | None = None,
    case_id: str | None = None,
    options: Options | None = None,
) -> list[dict[str, Any]]:
    """Enforce the observed-log lifecycle invariants (task
    hh-terminal-verb-assertions): exactly one SUCCESSFUL terminal verb
    (complete|fail|release|block) per claim token, every `heartbeat`
    carries `--ttl`, and no `claim`/`pull` is observed after a
    complete|fail|block on the same task (C5: `release` is excluded from
    this last gate -- it is a hand-back the ritual explicitly allows a
    re-claim to follow, including lapsed-claim recovery). Raises on the
    first violation found in (ts, pid, seq) order.

    Returns the C4 `terminal_attempt_failures` list: a FAILED terminal-verb
    attempt (e.g. a second `complete` the real binary rejected) does not
    itself violate "exactly one" and does not fail the run, but is
    surfaced here so the caller can record it as a grade-output warning --
    it is the same shape a genuine split-terminal defect would produce,
    just distinguished by exit code.
    """

    def fail(reason: str) -> None:
        if artifacts is not None and case_id is not None and options is not None:
            raise live_failure(artifacts, case_id, options, reason)
        raise EvalFailure(reason)

    terminal_calls_by_token: dict[str, list[dict[str, Any]]] = {}
    terminal_ts_by_task: dict[str, float] = {}
    terminal_attempt_failures: list[dict[str, Any]] = []
    ordered = sorted(observed_records, key=observed_sort_key)
    for record in ordered:
        argv = record.get("argv") or []
        if not argv:
            continue
        command = argv[0]
        ts = float(record.get("ts", 0))
        exit_code = record.get("exit_code")
        if command == "heartbeat":
            has_ttl = "--ttl" in argv or any(a.startswith("--ttl=") for a in argv)
            if not has_ttl:
                fail(f"heartbeat observed without --ttl: argv={argv!r}")
        if command in CLAIM_VERBS and exit_code == 0:
            task_id = observed_task_id(record)
            if (
                task_id is not None
                and task_id in terminal_ts_by_task
                and ts >= terminal_ts_by_task[task_id]
            ):
                fail(
                    "claim observed after a terminal verb on task "
                    f"{task_id}: argv={argv!r}"
                )
        if command in TERMINAL_VERBS:
            token = claim_token_of(argv) or observed_stdout_field(
                record, "claim_token"
            )
            if exit_code == 0:
                if token is not None:
                    terminal_calls_by_token.setdefault(token, []).append(record)
                if command in CLAIM_GATE_TERMINAL_VERBS:
                    task_id = observed_task_id(record)
                    if task_id is not None:
                        terminal_ts_by_task[task_id] = ts
            else:
                terminal_attempt_failures.append(
                    {
                        "command": command,
                        "claim_token": token,
                        "task_id": observed_task_id(record),
                        "argv": argv,
                        "exit_code": exit_code,
                        "stderr": record.get("stderr"),
                    }
                )
    for token, calls in terminal_calls_by_token.items():
        if len(calls) > 1:
            commands = [call.get("argv", [None])[0] for call in calls]
            fail(
                f"claim token {token} recorded {len(calls)} terminal verbs "
                f"({', '.join(str(c) for c in commands)}), expected exactly one"
            )
    return terminal_attempt_failures


def merge_lifecycle_events(
    observed_dir: Path, boundary_events_path: Path
) -> list[dict[str, Any]]:
    """Derive lifecycle event order from the observed log rather than
    reconstructing it from the audit table (decision D3). Wrapper-observed
    terminal verbs and the specialist fixtures' own boundary events
    (`coder-finished`, `review-approved`, ...) are two streams that
    interleave chronologically during a real run; each stream is already
    internally ordered, so a stable sort by (ts, pid, seq) merges them
    correctly, including a completion recorded before the reviewer
    boundary event. Boundary events carry no `pid`/`seq` of their own
    (0 stands in for both), so two records only ever tie-break against
    each other when they share the exact same `ts`.
    """
    merged: list[tuple[tuple[float, int, int], dict[str, Any]]] = []
    for record in read_observed_records(observed_dir):
        name = observed_event_name(record)
        if name is not None:
            merged.append(
                (
                    observed_sort_key(record),
                    {
                        "event": name,
                        "source": "planar-agent",
                        "ts": record.get("ts"),
                        "pid": record.get("pid"),
                        "seq": record.get("seq"),
                    },
                )
            )
    for index, event in enumerate(event_objects(boundary_events_path)):
        ts = event.get("ts")
        sort_key = (
            float(ts) if ts is not None else float(index),
            int(event.get("pid", 0)),
            int(event.get("seq", 0)),
        )
        merged.append((sort_key, event))
    merged.sort(key=lambda item: item[0])
    return [event for _, event in merged]


def collect_lifecycle_artifacts(
    context: LifecycleContext, options: Options
) -> None:
    case = context.case
    repo = context.repo
    artifacts = context.artifacts
    env = context.env
    task = run_json(
        ["planar", "task", "show", context.task_id, "--json"], cwd=repo, env=env
    )
    audit = run_json(
        ["planar", "audit", "trail", "--kind", "task", context.task_id, "--json"],
        cwd=repo,
        env=env,
    )
    write_json(artifacts / "task.after.json", task)
    write_json(artifacts / "task.audit.json", audit)
    observed_dir = repo / ".eval" / "observed"
    observed_records = read_observed_records(observed_dir)
    claims = audit.get("agent_activity", {}).get("claims", [])
    assert_no_recording_failures(
        observed_dir, artifacts=artifacts, case_id=case["id"], options=options
    )
    assert_wrapper_not_bypassed(
        observed_records,
        claims,
        task_id=context.task_id,
        artifacts=artifacts,
        case_id=case["id"],
        options=options,
    )
    terminal_attempt_failures = assert_terminal_verb_lifecycle(
        observed_records,
        artifacts=artifacts,
        case_id=case["id"],
        options=options,
    )
    write_lifecycle_warnings(artifacts, terminal_attempt_failures)
    events = merge_lifecycle_events(observed_dir, repo / ".eval" / "events.jsonl")
    write_text(
        artifacts / "events.normalized.jsonl",
        "".join(json.dumps(event, separators=(",", ":")) + "\n" for event in events),
    )
    write_json(artifacts / "events.normalized.json", events)
    claim_state = run_json(
        ["planar-watch", "ps", "--plan", context.plan_id, "--json"],
        cwd=repo,
        env=env,
    )
    write_json(artifacts / "claims.after.json", claim_state)
    test_result = run_command(["make", "test"], cwd=repo, env=env, check=False)
    write_text(
        artifacts / "fixture-test.txt", test_result.stdout + test_result.stderr
    )
    write_json(
        artifacts / "fixture-test.json",
        {"returncode": test_result.returncode},
    )
    if test_result.returncode != 0:
        raise live_failure(
            artifacts, case["id"], options_from_run(artifacts), "fixture tests failed"
        )
    write_text(
        artifacts / "git-status.txt",
        run_command(["git", "status", "--short"], cwd=repo, env=env).stdout,
    )
    write_text(
        artifacts / "git-graph.txt",
        run_command(
            ["git", "log", "--oneline", "--decorate", "--graph", "--all"],
            cwd=repo,
            env=env,
        ).stdout,
    )


def grade_lifecycle_artifacts(
    case: dict[str, Any], artifact_dir: Path, options: Options
) -> None:
    case_id = case["id"]
    required = [
        "events.normalized.json",
        "task.after.json",
        "claims.after.json",
        "fixture-test.txt",
        "fixture-test.json",
        "repo/src/value.txt",
    ]
    missing = [name for name in required if not (artifact_dir / name).is_file()]
    if missing:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"artifact set is incomplete: {', '.join(missing)}",
        )
    # Re-run the observed-log lifecycle assertions against the RETAINED
    # observed/ directory (it lives under artifact_dir/repo/.eval/observed
    # because `repo` is itself a subtree of the run's artifact dir), so a
    # `--grade-artifacts` regrade of a retained run re-derives the same
    # wrapper-bypassed / terminal-verb guarantees collect-time already
    # enforced rather than trusting a canned events.normalized.json that
    # could have been hand-edited or produced by a stale collector.
    # Artifact sets that predate C2 (or synthetic fixtures that never
    # populate a repo/.eval/observed tree) have no such directory; the
    # recheck is a no-op for those -- but ONLY when run.json's own
    # artifact_format says the run never promised one. A run.json at or
    # above ARTIFACT_FORMAT_OBSERVED_LOG with observed/ missing means a
    # post-C2 collection run failed to produce the tree it is supposed to
    # always write, and that is a grading failure, not a legacy gap (task
    # 6876: this used to silently no-op regardless of format).
    observed_dir = artifact_dir / "repo" / ".eval" / "observed"
    run_json_for_format = artifact_dir / "run.json"
    artifact_format = (
        read_json(run_json_for_format).get("artifact_format", 0)
        if run_json_for_format.is_file()
        else 0
    )
    if not observed_dir.is_dir():
        if artifact_format >= ARTIFACT_FORMAT_OBSERVED_LOG:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                "observed-log-missing: run.json declares artifact_format "
                f"{artifact_format} but repo/.eval/observed is absent",
            )
        # Legacy artifact set (format < ARTIFACT_FORMAT_OBSERVED_LOG, or no
        # artifact_format at all): no observed log was ever promised, skip.
    else:
        observed_records = read_observed_records(observed_dir)
        claims: list[dict[str, Any]] = []
        audit_path = artifact_dir / "task.audit.json"
        if audit_path.is_file():
            audit = read_json(audit_path)
            claims = audit.get("agent_activity", {}).get("claims", [])
        # C7: the audit's claims are task-scoped, so the count compared
        # against them must be too (see assert_wrapper_not_bypassed's own
        # docstring) -- `run.json` (also retained under artifact_dir)
        # carries the task id `prepare_lifecycle_fixture` recorded at
        # dispatch time.
        run_json_path = artifact_dir / "run.json"
        retained_task_id = (
            read_json(run_json_path).get("task_id")
            if run_json_path.is_file()
            else None
        )
        assert_no_recording_failures(
            observed_dir, artifacts=artifact_dir, case_id=case_id, options=options
        )
        assert_wrapper_not_bypassed(
            observed_records,
            claims,
            task_id=retained_task_id,
            artifacts=artifact_dir,
            case_id=case_id,
            options=options,
        )
        terminal_attempt_failures = assert_terminal_verb_lifecycle(
            observed_records,
            artifacts=artifact_dir,
            case_id=case_id,
            options=options,
        )
        write_lifecycle_warnings(artifact_dir, terminal_attempt_failures)
    events = read_json(artifact_dir / "events.normalized.json")
    names = [event.get("event") for event in events]
    cursor = -1
    for expected_event in case.get("expected", {}).get("ordered_events", []):
        try:
            cursor = names.index(expected_event, cursor + 1)
        except ValueError:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"ordered event missing after index {cursor}: {expected_event}",
            )
    for forbidden in case.get("expected", {}).get("forbidden_events", []):
        if forbidden in names:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"forbidden event observed: {forbidden}",
            )
    for event, expected_count in case.get("expected", {}).get(
        "event_counts", {}
    ).items():
        actual = names.count(event)
        if actual != expected_count:
            raise live_failure(
                artifact_dir,
                case_id,
                options,
                f"event {event} count was {actual}, expected {expected_count}",
            )
    # `task_status` and `file_value` are REQUIRED on every lifecycle case's
    # `expected.post_state` (validate_case, task 6837/D4): there is no
    # longer a silent-skip branch here for either key being absent, because
    # a lifecycle case can no longer reach this function without them.
    post_state = case.get("expected", {}).get("post_state", {})
    task = read_json(artifact_dir / "task.after.json")
    expected_status = post_state["task_status"]
    if task.get("status") != expected_status:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"task status was {task.get('status')}, expected {expected_status}",
        )
    claims = read_json(artifact_dir / "claims.after.json")
    active_count = len(claims.get("active", []))
    expected_claims = post_state.get("active_claims")
    if expected_claims is not None and active_count != expected_claims:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"active claim count was {active_count}, expected {expected_claims}",
        )
    actual_value = (artifact_dir / "repo" / "src" / "value.txt").read_text(
        encoding="utf-8"
    ).strip()
    expected_value = post_state["file_value"]
    if actual_value != expected_value:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"fixture value was {actual_value}, expected {expected_value}",
        )
    test_result = read_json(artifact_dir / "fixture-test.json")
    if test_result.get("returncode") != 0:
        raise live_failure(
            artifact_dir,
            case_id,
            options,
            f"fixture test exit code was {test_result.get('returncode')}, expected 0",
        )


def run_lifecycle_fixture_replay(
    case_path: Path,
    case: dict[str, Any],
    options: Options,
    *,
    seed_violation: str | None = None,
) -> None:
    """Replay the controlled-classic fixture end-to-end and grade it.

    `seed_violation`, when set, is forwarded as `EVAL_SEED_VIOLATION` to
    every `.eval/control.sh` invocation (task 6892). The fixture's own
    knob (see `control.sh`) then emits the named event, so this same code
    path -- not a synthetic events.jsonl the grader has never actually
    produced -- is what `run_lifecycle_fixture_negative_control` drives to
    prove `grade_lifecycle_artifacts` can fail a real run, not just the
    two isolated unit halves (emitting the event, and rejecting a
    hand-built event list) that existed before.
    """
    label = "fixture-negative-control" if seed_violation else "fixture"
    context = prepare_lifecycle_fixture(case_path, case, options, label)
    if seed_violation:
        context.env["EVAL_SEED_VIOLATION"] = seed_violation
    try:
        claim = run_json(
            [
                "planar-agent",
                "pull",
                context.plan_id,
                "--role",
                "coder",
                "--base-ref",
                "HEAD",
                "--repo-root",
                str(context.repo),
                "--json",
            ],
            cwd=context.repo,
            env=context.env,
        )
        claim_token = (
            claim.get("claim_token")
            or claim.get("claim", {}).get("claim_token")
            or claim.get("claim", {}).get("token")
        )
        if not claim_token:
            raise live_failure(
                context.artifacts,
                case["id"],
                options,
                "fixture replay did not receive a claim token",
            )
        coder = run_command(
            ["./.eval/control.sh", "coder", str(claim_token)],
            cwd=context.repo,
            env=context.env,
        )
        write_text(context.artifacts / "coder-1.txt", coder.stdout)
        reviewer = run_command(
            ["./.eval/control.sh", "reviewer", str(claim_token)],
            cwd=context.repo,
            env=context.env,
        )
        write_text(context.artifacts / "reviewer-1.txt", reviewer.stdout)
        if "decision: request-changes" in reviewer.stdout:
            coder2 = run_command(
                ["./.eval/control.sh", "coder", str(claim_token)],
                cwd=context.repo,
                env=context.env,
            )
            reviewer2 = run_command(
                ["./.eval/control.sh", "reviewer", str(claim_token)],
                cwd=context.repo,
                env=context.env,
            )
            write_text(context.artifacts / "coder-2.txt", coder2.stdout)
            write_text(context.artifacts / "reviewer-2.txt", reviewer2.stdout)
        complete = run_command(
            [
                "planar-agent",
                "complete",
                "--claim",
                str(claim_token),
                "--summary",
                "controlled lifecycle fixture approved",
                "--json",
            ],
            cwd=context.repo,
            env=context.env,
        )
        write_text(context.artifacts / "complete.json", complete.stdout)
        collect_lifecycle_artifacts(context, options)
        grade_lifecycle_artifacts(case, context.artifacts, options)
        write_grade(context.artifacts, "pass", case["id"], options)
        pass_line(f"{case['id']}: controlled lifecycle fixture replay")
        finish_artifacts(context.artifacts, options)
    except (EvalFailure, EvalBlocked) as exc:
        if exc.artifacts is None:
            exc.artifacts = context.artifacts
        raise


# The fixture's seeded-violation knob (control.sh) only ever emits this one
# event; kept as a single named constant so the negative control and its
# candidate search below cannot drift from the string `control.sh` reads.
LIFECYCLE_SEEDED_VIOLATION = "task-completed-before-review"


def run_lifecycle_fixture_negative_control(
    cases: list[tuple[Path, dict[str, Any]]], options: Options
) -> None:
    """Task 6892: prove the fixture-replay lane can actually FAIL a run,
    end-to-end, not just its two previously-isolated unit halves (a test
    that the fixture emits the seeded event, and a separate test that
    feeds a hand-built event list into `grade_lifecycle_artifacts`).

    Runs a REAL `run_lifecycle_fixture_replay` -- claim, controlled
    coder/reviewer, complete, collect, grade -- against the real
    controlled-classic fixture in the arena, with
    `EVAL_SEED_VIOLATION=task-completed-before-review` forcing the fixture
    to emit the one event every repository lifecycle case's
    `forbidden_events` already forbids. The replay is REQUIRED to raise
    `EvalFailure` naming that forbidden event; anything else (no raise, or
    a raise for an unrelated reason) is itself a suite failure, so this
    is a fail-closed negative control, not a smoke test that only proves
    the happy path runs.
    """
    seeded = LIFECYCLE_SEEDED_VIOLATION
    candidates = [
        (path, case)
        for path, case in cases
        if "lifecycle" in case.get("tiers", [])
        and seeded in case.get("expected", {}).get("forbidden_events", [])
    ]
    if not candidates:
        raise EvalFailure(
            "lifecycle fixture negative control needs a repository case "
            f"whose expected.forbidden_events declares {seeded!r}"
        )
    path, case = candidates[0]
    try:
        run_lifecycle_fixture_replay(path, case, options, seed_violation=seeded)
    except EvalFailure as exc:
        if f"forbidden event observed: {seeded}" not in str(exc):
            raise EvalFailure(
                "lifecycle fixture negative control failed for the wrong "
                f"reason (expected a rejected forbidden event {seeded!r}): {exc}"
            ) from exc
        if exc.artifacts is not None and not options.keep and options.results_dir is None:
            shutil.rmtree(exc.artifacts, ignore_errors=True)
        return
    raise EvalFailure(
        f"lifecycle fixture negative control was not detected: {case['id']} "
        f"did not fail under EVAL_SEED_VIOLATION={seeded}"
    )


def append_file(target: Path, source: Path) -> None:
    with target.open("ab") as output, source.open("rb") as incoming:
        output.write(b"\n")
        shutil.copyfileobj(incoming, output)


def run_lifecycle_host_from_prepared(
    context: LifecycleContext, case: dict[str, Any], options: Options
) -> None:
    """RUN + GRADE steps (task 6849 split) for a lifecycle host run, given a
    `LifecycleContext` `prepare_lifecycle_fixture` already produced (that
    function IS this mode's prepare step -- it writes `run.json` and never
    invokes a vendor host itself).
    """
    artifacts = context.artifacts
    raw = artifacts / "transcript.jsonl"
    timeout = int(case["lifecycle"].get("timeout_seconds", 600))
    prompt = case["lifecycle"]["prompt"].replace("{{PLAN_ID}}", context.plan_id)
    allowed_tools = resolve_allowed_tools(case["lifecycle"])
    case_budget_usd = case["lifecycle"].get("budget", {}).get("max_usd")
    usage_total: dict[str, Any] = {
        "calls": 0,
        "total_cost_usd": None,
        "input_tokens": 0,
        "output_tokens": 0,
    }
    try:
        print(
            f"RUN: {case['id']} ({options.vendor}/{options.surface} lifecycle), "
            f"timeout {timeout}s per turn",
            flush=True,
        )
        print(f"transcript: {raw}", flush=True)
        print(f"follow: tail -f {raw}", flush=True)
        instructions = (
            context.repo / ".eval" / "orchestrator.instructions.md"
        ).read_text(encoding="utf-8")
        encoded_instructions = json.dumps(instructions)
        command = vendors.build_lifecycle_command(
            vendor=options.vendor,
            prompt=prompt,
            encoded_instructions=encoded_instructions,
            allowed_tools=allowed_tools,
        )
        rc = run_to_file(
            command, raw, cwd=context.repo, env=context.env, timeout_seconds=timeout
        )
        raw_text = raw.read_text(encoding="utf-8", errors="replace")
        limited = rate_limit_reason(raw_text)
        if limited:
            write_grade(artifacts, "blocked", case["id"], options, limited)
            raise EvalBlocked(
                f"{case['id']}: {options.vendor}/{options.surface} {limited}",
                artifacts,
            )
        if rc == 124:
            raise live_failure(
                artifacts,
                case["id"],
                options,
                f"{options.vendor}/{options.surface} lifecycle timed out after {timeout}s",
            )
        if rc != 0:
            raise live_failure(
                artifacts,
                case["id"],
                options,
                f"{options.vendor}/{options.surface} lifecycle invocation exited {rc}",
            )
        usage_total = merge_usage(usage_total, extract_usage(raw, options.vendor))
        session_id = extract_session_id(raw, options.vendor)
        if not session_id:
            raise live_failure(
                artifacts,
                case["id"],
                options,
                "host transcript omitted resumable session id",
            )
        responses = case["lifecycle"].get("operator_responses", [])
        task_status = run_json(
            ["planar", "task", "show", context.task_id, "--json"],
            cwd=context.repo,
            env=context.env,
        ).get("status")
        for turn, response in enumerate(responses, start=2):
            if task_status in {"done", "cancelled"}:
                break
            turn_raw = artifacts / f"transcript.turn-{turn}.jsonl"
            command = vendors.build_lifecycle_resume_command(
                vendor=options.vendor,
                session_id=session_id,
                response=response,
                encoded_instructions=encoded_instructions,
                allowed_tools=allowed_tools,
            )
            rc = run_to_file(
                command,
                turn_raw,
                cwd=context.repo,
                env=context.env,
                timeout_seconds=timeout,
            )
            append_file(raw, turn_raw)
            turn_text = turn_raw.read_text(encoding="utf-8", errors="replace")
            limited = rate_limit_reason(turn_text)
            if limited:
                write_grade(
                    artifacts,
                    "blocked",
                    case["id"],
                    options,
                    f"{limited} on turn {turn}",
                )
                raise EvalBlocked(
                    f"{case['id']}: {options.vendor}/{options.surface} "
                    f"{limited} on turn {turn}",
                    artifacts,
                )
            if rc == 124:
                raise live_failure(
                    artifacts,
                    case["id"],
                    options,
                    f"{options.vendor}/{options.surface} lifecycle turn {turn} "
                    f"timed out after {timeout}s",
                )
            if rc != 0:
                raise live_failure(
                    artifacts,
                    case["id"],
                    options,
                    f"{options.vendor}/{options.surface} lifecycle turn {turn} exited {rc}",
                )
            usage_total = merge_usage(
                usage_total, extract_usage(turn_raw, options.vendor)
            )
            task_status = run_json(
                ["planar", "task", "show", context.task_id, "--json"],
                cwd=context.repo,
                env=context.env,
            ).get("status")
        enforce_spend_ceiling(
            artifact_dir=artifacts,
            case_id=case["id"],
            options=options,
            repo=context.repo,
            env=context.env,
            plan_id=context.plan_id,
            usage=usage_total,
            case_budget_usd=case_budget_usd,
        )
        extract_host_transcript(raw, options.vendor, artifacts)
        collect_lifecycle_artifacts(context, options)
        grade_lifecycle_artifacts(case, artifacts, options)
        write_grade(artifacts, "pass", case["id"], options, usage=usage_total)
        pass_line(
            f"{case['id']}: {options.vendor}/{options.surface} controlled lifecycle"
        )
        finish_artifacts(artifacts, options)
    except (EvalFailure, EvalBlocked) as exc:
        if exc.artifacts is None:
            exc.artifacts = artifacts
        raise


def run_lifecycle_host(
    case_path: Path, case: dict[str, Any], options: Options
) -> None:
    """Single-step controlled-lifecycle host run: prepare, then run+grade."""
    context = prepare_lifecycle_fixture(
        case_path, case, options, f"{options.vendor}-{options.surface}"
    )
    run_lifecycle_host_from_prepared(context, case, options)


def options_from_run(artifact_dir: Path) -> Options:
    run_path = artifact_dir / "run.json"
    grade_path = artifact_dir / "grade.json"
    metadata = read_json(run_path if run_path.exists() else grade_path)
    return Options(
        mode=metadata.get("mode", "live"),
        vendor=metadata.get("vendor", "codex"),
        surface=metadata.get("surface", "skill"),
        case_filter=metadata.get("case_id"),
        results_dir=artifact_dir.parent,
        keep=True,
    )


# Task 6860: the results ledger. Append-only, committed to the repo (not
# a runtime artifact); `regrade_artifacts` (the `--grade-artifacts` entry
# point) is the sole writer, matching "grade_*_artifacts(artifact_dir) is
# the single grading entry point" from the task-6849 split -- a live or
# lifecycle run only reaches the ledger once an operator regrades its
# retained artifacts.
RESULTS_LEDGER_PATH = ROOT / "evals" / "RESULTS.md"
LEDGER_TABLE_HEADER = (
    "| date | case | mode | vendor | surface | model | grade | artifact hash |\n"
    "|---|---|---|---|---|---|---|---|\n"
)
LEDGER_PREAMBLE = (
    "# Orchestrator eval results ledger\n\n"
    "Append-only record of graded `live`/`lifecycle` orchestrator eval runs. "
    "One row per `--grade-artifacts <dir>` regrade (see `regrade_artifacts` "
    "in `harness.py`). A blocked run (host exit 75) is recorded with grade "
    "`blocked` and never counts as a pass. Never hand-edit an existing row; "
    "append a new one instead -- the row for a given artifact hash is the "
    "regrade's own verdict at that point in time, and a later regrade of "
    "the same retained artifacts adds another row rather than replacing it.\n\n"
    + LEDGER_TABLE_HEADER
)
_LEDGER_DEFAULT_MAX_AGE_DAYS = 30


def ledger_model_label(case: dict[str, Any], vendor: str) -> str:
    """Best-effort model label for a ledger row. Lifecycle cases carry no
    explicit model field in the schema, so lifecycle rows fall back to `-`.
    """
    live = case.get("live") or {}
    host_model = live.get("host_model")
    if isinstance(host_model, str) and host_model:
        return host_model
    expected = live.get("expected_models") or {}
    value = expected.get(vendor)
    return value if isinstance(value, str) and value else "-"


def artifact_content_hash(artifact_dir: Path) -> str:
    """Short, stable hash identifying this artifact set's graded content."""
    grade_path = artifact_dir / "grade.json"
    payload = grade_path.read_bytes() if grade_path.exists() else b""
    return hashlib.sha256(payload).hexdigest()[:12]


def retained_grade_status(artifact_dir: Path) -> str | None:
    """The `status` a retained run recorded in its own `grade.json`, or
    `None` when there is none to read (task 6852/6860). Used so a regrade
    cannot promote an aborted run to a pass.
    """
    path = artifact_dir / "grade.json"
    if not path.is_file():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    status = value.get("status") if isinstance(value, dict) else None
    return status if isinstance(status, str) else None


def record_ledger_row(
    case: dict[str, Any], artifact_dir: Path, options: Options, grade: str
) -> None:
    """Append one row to `evals/RESULTS.md`. Never rewrites or removes an
    existing row -- append-only, per task 6860.
    """
    RESULTS_LEDGER_PATH.parent.mkdir(parents=True, exist_ok=True)
    existing = (
        RESULTS_LEDGER_PATH.read_text(encoding="utf-8")
        if RESULTS_LEDGER_PATH.exists()
        else LEDGER_PREAMBLE
    )
    if not existing.endswith("\n"):
        existing += "\n"
    date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    row = (
        f"| {date} | {case['id']} | {options.mode} | {options.vendor or '-'} | "
        f"{options.surface or '-'} | {ledger_model_label(case, options.vendor)} | "
        f"{grade} | {artifact_content_hash(artifact_dir)} |\n"
    )
    RESULTS_LEDGER_PATH.write_text(existing + row, encoding="utf-8")


def ledger_rows() -> list[dict[str, str]]:
    """Parse `evals/RESULTS.md` into row dicts keyed by the table header.
    Returns an empty list when the ledger does not exist yet (a fresh
    checkout before any run has ever been regraded).
    """
    if not RESULTS_LEDGER_PATH.exists():
        return []
    lines = RESULTS_LEDGER_PATH.read_text(encoding="utf-8").splitlines()
    data_lines = [
        line
        for line in lines
        if line.startswith("|") and not set(line.replace("|", "").strip()) <= {"-"}
    ]
    if len(data_lines) < 2:
        return []
    header = [cell.strip() for cell in data_lines[0].strip("|").split("|")]
    rows: list[dict[str, str]] = []
    for line in data_lines[1:]:
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if len(cells) != len(header):
            continue
        rows.append(dict(zip(header, cells)))
    return rows


def ledger_max_age_days(case: dict[str, Any]) -> int:
    """The freshness window `make eval-ledger-check` enforces for `case`.
    Checked on whichever of `live`/`lifecycle` the case declares; a case
    declaring both is checked against each section's own value.
    """
    for section_name in ("live", "lifecycle"):
        section = case.get(section_name)
        if isinstance(section, dict):
            ledger_cfg = section.get("ledger")
            if isinstance(ledger_cfg, dict) and "max_age_days" in ledger_cfg:
                return int(ledger_cfg["max_age_days"])
    return _LEDGER_DEFAULT_MAX_AGE_DAYS


def check_ledger_freshness(
    cases: list[tuple[Path, dict[str, Any]]]
) -> list[str]:
    """Return one violation string per case declaring `live`/`lifecycle`
    that has no non-`blocked` ledger row newer than its
    `ledger.max_age_days` (task 6860). Empty list means the ledger is
    current for every such case.
    """
    rows = ledger_rows()
    now = datetime.now(timezone.utc)
    violations: list[str] = []
    for _, case in cases:
        tiers = case.get("tiers", [])
        if "live" not in tiers and "lifecycle" not in tiers:
            continue
        max_age = ledger_max_age_days(case)
        freshest: datetime | None = None
        for row in rows:
            if row.get("case") != case["id"] or row.get("grade") == "blocked":
                continue
            try:
                stamp = datetime.strptime(row["date"], "%Y-%m-%d").replace(
                    tzinfo=timezone.utc
                )
            except (KeyError, ValueError):
                continue
            if freshest is None or stamp > freshest:
                freshest = stamp
        if freshest is None:
            violations.append(
                f"{case['id']}: no non-blocked ledger row recorded in "
                f"{RESULTS_LEDGER_PATH}"
            )
            continue
        age_days = (now - freshest).days
        if age_days > max_age:
            violations.append(
                f"{case['id']}: newest non-blocked ledger row is {age_days}d old, "
                f"exceeds ledger.max_age_days {max_age}"
            )
    return violations


def regrade_artifact_path(artifact_dir: Path) -> Path:
    """Return the dated sibling path a regrade writes to (C6 / task 6840),
    e.g. `grade.20260922T161530123456Z.json` next to the original
    `grade.json`. Microsecond precision keeps back-to-back regrades of the
    same retained run from colliding on the same filename.
    """
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return artifact_dir / f"grade.{stamp}.json"


def regrade_artifacts(
    artifact_dir: Path, cases: list[tuple[Path, dict[str, Any]]]
) -> Path:
    """Regrade a retained run's artifacts. Writes the fresh verdict to a
    new dated file alongside the original `grade.json` and leaves that
    original untouched (C6 / task 6840) -- a regrade that disagrees with
    the retained run must not silently overwrite the verdict the run
    actually produced. Returns the path written.
    """
    artifact_dir = artifact_dir.resolve()
    options = options_from_run(artifact_dir)
    metadata_path = (
        artifact_dir / "run.json"
        if (artifact_dir / "run.json").exists()
        else artifact_dir / "grade.json"
    )
    metadata = read_json(metadata_path)
    case_id = metadata.get("case_id")
    matches = [case for _, case in cases if case["id"] == case_id]
    if len(matches) != 1:
        raise EvalFailure(f"cannot resolve case for retained artifacts: {case_id}")
    case = matches[0]
    try:
        if options.mode == "live":
            grade_live_artifacts(case, artifact_dir, options)
        elif options.mode in {"lifecycle", "lifecycle-fixture"}:
            grade_lifecycle_artifacts(case, artifact_dir, options)
        else:
            raise EvalFailure(f"artifact mode is not regradable: {options.mode}")
    except (EvalFailure, EvalBlocked) as exc:
        # Task 6860: the ledger records every graded live/lifecycle run, not
        # only the ones that pass -- a regrade that disagrees with the
        # retained run's own verdict is itself evidence, not noise to drop.
        if options.mode in {"live", "lifecycle"}:
            status = "blocked" if isinstance(exc, EvalBlocked) else "fail"
            record_ledger_row(case, artifact_dir, options, status)
        raise
    # A retained run that stopped on its spend ceiling never produced a
    # gradeable result: the graders above only inspect the artifacts that
    # DID land, so they can return clean and turn an aborted run into a
    # `pass` row. The retained verdict wins (reviewer, M3 cycle 1).
    retained = retained_grade_status(artifact_dir)
    verdict = "over-budget" if retained == "over-budget" else "pass"
    regrade_path = write_grade(
        artifact_dir, verdict, case_id, options, target=regrade_artifact_path(artifact_dir)
    )
    if options.mode in {"live", "lifecycle"}:
        record_ledger_row(case, artifact_dir, options, verdict)
    if verdict == "over-budget":
        raise EvalFailure(
            f"{case_id}: retained artifacts are from an over-budget run; not a pass", artifact_dir
        )
    pass_line(f"{case_id}: retained {options.mode} artifacts regraded")
    print(f"regraded: {regrade_path}")
    print(f"artifacts: {artifact_dir}")
    return regrade_path


def parse_args(
    argv: Sequence[str],
) -> tuple[Options, bool, Path | None, bool, bool]:
    parser = argparse.ArgumentParser(
        description="Run Planar orchestrator contract, live, and lifecycle evals."
    )
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--contract-only", action="store_true")
    modes.add_argument("--live", action="store_true")
    modes.add_argument("--lifecycle", action="store_true")
    modes.add_argument("--lifecycle-fixture-only", action="store_true")
    parser.add_argument("--vendor", default="")
    parser.add_argument("--surface", default="skill")
    parser.add_argument("--case", dest="case_filter")
    parser.add_argument("--results", type=Path)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--grade-artifacts", type=Path)
    # Task 6860: report every case declaring `live`/`lifecycle` whose ledger
    # row is missing or stale, without regrading anything.
    parser.add_argument("--ledger-check", action="store_true")
    # Task 6849: stop after building the arena/plan/tasks and writing
    # run.json, before any vendor host is invoked.
    parser.add_argument("--prepare", action="store_true")
    # Task 6852: suite-level cost ceiling, checked cumulatively across every
    # case run in this invocation (see Options.suite_spend_usd).
    parser.add_argument("--max-usd", type=float, dest="max_usd")
    # Task 6859: repeat each selected live/lifecycle case this many times and
    # report a per-case pass rate.
    parser.add_argument("--trials", type=int, default=1)
    args = parser.parse_args(argv)
    if args.live:
        mode = "live"
    elif args.lifecycle:
        mode = "lifecycle"
    elif args.lifecycle_fixture_only:
        mode = "lifecycle-fixture"
    else:
        mode = "contract"
    if args.trials < 1:
        raise EvalFailure("--trials must be >= 1")
    options = Options(
        mode=mode,
        vendor=args.vendor,
        surface=args.surface,
        case_filter=args.case_filter,
        results_dir=args.results.resolve() if args.results else None,
        keep=args.keep,
        max_usd=args.max_usd,
        trials=args.trials,
    )
    return options, args.list, args.grade_artifacts, args.prepare, args.ledger_check


def run_case_trials(
    entrypoint: Callable[[Path, dict[str, Any], Options], None],
    path: Path,
    case: dict[str, Any],
    options: Options,
) -> None:
    """Run `entrypoint` `options.trials` times for one case and report a
    per-case pass rate (task 6859). `options.trials == 1` (the default)
    calls `entrypoint` directly with no wrapper overhead, so single-run
    behavior is unchanged from before task 6859.

    A trial that raises `EvalFailure`/`EvalBlocked` is recorded and does
    NOT stop the remaining trials. `EvalOverBudget` is the exception to
    that: it propagates immediately, because a spend ceiling governs the
    whole invocation rather than one trial. Only a 0/N pass rate re-raises (the
    last trial's exception), so a fully-failing case still fails the
    overall run; a partially-flaky case reports its rate and succeeds.
    """
    if options.trials == 1:
        entrypoint(path, case, options)
        return
    outcomes: list[dict[str, Any]] = []
    passed = 0
    last_exc: EvalFailure | EvalBlocked | None = None
    for trial in range(1, options.trials + 1):
        try:
            entrypoint(path, case, options)
        except EvalOverBudget:
            # A ceiling is not a per-trial outcome: the remaining trials
            # would keep spending past the number the operator set, and a
            # later passing trial would return 0 and hide it. `--trials` is
            # the mode MOST likely to overspend, so the abort has to escape
            # the loop it is nested in (task 6852; reviewer, M3 cycle 1).
            raise
        except (EvalFailure, EvalBlocked) as exc:
            status = "blocked" if isinstance(exc, EvalBlocked) else "fail"
            outcomes.append({"trial": trial, "status": status, "reason": str(exc)})
            last_exc = exc
        else:
            passed += 1
            outcomes.append({"trial": trial, "status": "pass"})
    pass_rate = passed / options.trials
    summary = {
        "case_id": case["id"],
        "trials": options.trials,
        "passed": passed,
        "pass_rate": pass_rate,
        "outcomes": outcomes,
    }
    if options.results_dir is not None:
        summary_dir = options.results_dir / case["id"]
        summary_dir.mkdir(parents=True, exist_ok=True)
        write_json(summary_dir / "trials.json", summary)
    print(
        f"TRIALS: {case['id']} {passed}/{options.trials} passed "
        f"(pass_rate={pass_rate:.2f})",
        flush=True,
    )
    if passed == 0 and last_exc is not None:
        raise last_exc


def main(argv: Sequence[str] | None = None) -> int:
    options, list_only, regrade_path, prepare_only, ledger_check_only = parse_args(
        argv or sys.argv[1:]
    )
    cases = load_cases()
    if ledger_check_only:
        violations = check_ledger_freshness(cases)
        if violations:
            for line in violations:
                print(f"STALE: {line}", file=sys.stderr)
            raise EvalFailure(
                f"{len(violations)} case(s) have a missing or stale ledger row"
            )
        pass_line(f"{len(cases)} case(s) checked against {RESULTS_LEDGER_PATH.name}")
        return 0
    if regrade_path is not None:
        regrade_artifacts(regrade_path, cases)
        return 0
    selected = select_cases(cases, options.case_filter)
    if list_only:
        for _, case in selected:
            print(f"{case['id']}\t{','.join(case['tiers'])}\t{case['description']}")
        return 0
    if options.mode == "contract":
        grade_coherence()
        pass_line("orchestrator cross-role coherence")
        grade_coherence_negative_control()
        pass_line("coherence grader negative control")
        grade_contract_negative_control()
        pass_line("contract grader negative control")
        contract_entries = [
            (path, case) for path, case in selected if "contract" in case["tiers"]
        ]
        contract_outcomes = collect_case_failures(contract_entries, grade_contract)
        emit_aggregate_report(
            contract_outcomes,
            total=len(contract_entries),
            unit="orchestrator case definitions and deterministic contracts",
            pass_message=(
                f"{len(selected)} orchestrator case definitions and "
                "deterministic contracts"
            ),
        )
        self_tests_run, total_assertions, selftest_outcomes = run_assertion_selftests(
            selected
        )
        emit_aggregate_report(
            selftest_outcomes,
            total=total_assertions,
            unit="contract assertions with a falsifiable seeded-violation self-test",
            pass_message=(
                f"{self_tests_run} of {total_assertions} contract assertions have a "
                "falsifiable seeded-violation self-test"
            ),
        )
        return batch_exit_code(contract_outcomes + selftest_outcomes)
    if options.mode == "live":
        if options.vendor not in {"codex", "claude"}:
            raise EvalFailure("--vendor must be codex or claude")
        if options.surface not in {"skill", "agent"}:
            raise EvalFailure("--surface must be skill or agent")
        require_commands(["planar", "planar-watch", options.vendor, "git"])
        require_current_installed_projection()
        runnable = [entry for entry in selected if "live" in entry[1]["tiers"]]
        if not runnable:
            raise EvalFailure("no selected cases declare the live tier")
        if prepare_only:
            for path, case in runnable:
                artifact_dir = prepare_phase3_preview(path, case, options)
                print(f"PREPARED: {case['id']} {artifact_dir}", flush=True)
            pass_line(f"{len(runnable)} live orchestrator cases prepared")
            return 0
        for path, case in runnable:
            run_case_trials(run_phase3_preview, path, case, options)
        pass_line(f"{len(runnable)} live orchestrator cases")
        return 0
    if options.mode == "lifecycle-fixture":
        require_commands(["planar", "planar-agent", "planar-watch", "git"])
        runnable = [entry for entry in selected if "lifecycle" in entry[1]["tiers"]]
        if not runnable:
            raise EvalFailure("no selected cases declare the lifecycle tier")
        # Task 6892: proves the fixture-replay lane can actually FAIL a run
        # before trusting the pass-only loop below, the same way the
        # contract lane's negative controls run before its own pass loop.
        # Uses `cases` (every loaded case), not `selected`, so a `--case`
        # filter narrowing `runnable` never skips this control.
        run_lifecycle_fixture_negative_control(cases, options)
        pass_line("lifecycle fixture negative control")
        fixture_outcomes = collect_case_failures(
            runnable,
            lambda path, case: run_lifecycle_fixture_replay(path, case, options),
        )
        emit_aggregate_report(
            fixture_outcomes,
            total=len(runnable),
            unit="controlled lifecycle fixture replays",
            pass_message=f"{len(runnable)} controlled lifecycle fixture replays",
        )
        return batch_exit_code(fixture_outcomes)
    if options.vendor not in {"codex", "claude"}:
        raise EvalFailure("--vendor must be codex or claude")
    if options.surface != "agent":
        raise EvalFailure("lifecycle mode currently supports --surface agent")
    require_commands(["planar", "planar-agent", "planar-watch", "git", options.vendor])
    require_current_installed_projection()
    runnable = [entry for entry in selected if "lifecycle" in entry[1]["tiers"]]
    if not runnable:
        raise EvalFailure("no selected cases declare the lifecycle tier")
    if prepare_only:
        for path, case in runnable:
            context = prepare_lifecycle_fixture(
                path, case, options, f"{options.vendor}-{options.surface}"
            )
            print(f"PREPARED: {case['id']} {context.artifacts}", flush=True)
        pass_line(f"{len(runnable)} live lifecycle orchestrator cases prepared")
        return 0
    for path, case in runnable:
        run_case_trials(run_lifecycle_host, path, case, options)
    pass_line(f"{len(runnable)} live lifecycle orchestrator cases")
    return 0


def cli_main(argv: Sequence[str] | None = None) -> int:
    """Run `main()` and map every fail-closed exception to an exit code.

    `EvalBlocked` / `EvalFailure` are the harness's own exit-coded failure
    types. `arena.ArenaIsolationError`, `arena.VendorStagingError`, and
    `arena.VendorAuthError` are the fail-closed arena checks
    (`assert_isolated` / `stage_vendor_config` / `assert_vendor_auth`) that
    raise before any vendor host starts; they are routed through the same
    exit-coded failure path as `EvalFailure` rather than left to propagate
    as a bare traceback (task 6885) -- a message to stderr and a nonzero
    exit, never a token value.
    """
    try:
        return main(argv)
    except EvalBlocked as exc:
        print(f"BLOCKED: {exc}", file=sys.stderr)
        if exc.artifacts:
            print(f"artifacts retained: {exc.artifacts}", file=sys.stderr)
        return 75
    except EvalFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        if exc.artifacts:
            print(f"artifacts retained: {exc.artifacts}", file=sys.stderr)
        return 1
    except (
        arena.ArenaIsolationError,
        arena.VendorStagingError,
        arena.VendorAuthError,
    ) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(cli_main())
