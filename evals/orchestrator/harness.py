#!/usr/bin/env python3
"""Planar orchestrator evaluation harness.

The harness deliberately uses only the Python standard library. Repository
fixtures that emulate external CLIs remain executable shell test doubles.
"""

from __future__ import annotations

import argparse
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
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

import arena

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
CLAUDE_LIVE_ALLOWED_TOOLS = "Bash,Read,Edit,Write,Task"
# Passed as ONE argv element. `--allowedTools` is variadic, so the two-token
# form swallows the prompt positional that follows it and the CLI dies with
# "Input must be provided ... when using --print".
CLAUDE_LIVE_ALLOWED_TOOLS_ARG = f"--allowedTools={CLAUDE_LIVE_ALLOWED_TOOLS}"


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


@dataclass
class Options:
    mode: str
    vendor: str
    surface: str
    case_filter: str | None
    results_dir: Path | None
    keep: bool


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


def grade_contract(case_path: Path, case: dict[str, Any]) -> None:
    case_id = case["id"]
    for assertion in case["contract_assertions"]:
        assertion_id = assertion["id"]
        matched = 0
        paths = assertion["paths"]
        for relative in paths:
            path = ROOT / relative
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
        "skills/src/pl-orchestrator.md": 2650,
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
) -> None:
    write_json(
        artifact_dir / "grade.json",
        {
            "status": status,
            "case_id": case_id,
            "vendor": options.vendor,
            "surface": options.surface,
            "mode": options.mode,
            "reason": reason,
        },
    )


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


def model_argv(host_model: str | None) -> list[str]:
    """Argv fragment selecting the orchestration host model.

    Returned as separate elements so the value is never re-parsed. Building a
    string here (`f"--model {host_model}"`) is what would turn an opaque id
    into shell syntax.
    """
    return ["--model", host_model] if host_model else []


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


def run_phase3_preview(
    case_path: Path, case: dict[str, Any], options: Options
) -> None:
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
    # any host process if a required surface is missing.
    arena.stage_vendor_config(env, options.vendor)
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

        prompt = case["live"]["prompt"].replace("{{PLAN_ID}}", plan_id)
        # Independent structured inputs, validated before they can reach a
        # command line. `delegated_candidate` is what the orchestrator should
        # dispatch a child role to; it is graded from the transcript rather
        # than forced here, because forcing it would test the harness instead
        # of the orchestrator's own routing.
        host_model, _delegated_candidate = resolve_model_inputs(case["live"])
        raw = artifact_dir / "transcript.jsonl"
        timeout = int(case["live"].get("timeout_seconds", 300))
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
        print(
            f"RUN: {case_id} ({options.vendor}/{options.surface}), timeout {timeout}s",
            flush=True,
        )
        print(f"transcript: {raw}", flush=True)
        print(f"follow: tail -f {raw}", flush=True)

        if options.vendor == "codex":
            if options.surface == "agent":
                adapter = (
                    "Do not read or invoke any skill. Immediately spawn the installed "
                    "orchestrator subagent with agent_type=orchestrator and "
                    "fork_turns=none, then delegate this entire task to it. An explicit "
                    "agent type must not use a full-history fork. Do not execute the "
                    "orchestration workflow in the parent agent. After the child returns, "
                    "emit its complete final response verbatim: do not summarize, "
                    "paraphrase, compress task ids into ranges, or omit task slugs. The "
                    "relayed preview must preserve one explicit id-and-slug row for every "
                    "task. If the child omitted that identity, ask it to correct the "
                    f"response before returning. {prompt}"
                )
            else:
                adapter = f"$orchestrator {plan_id}\n\n{prompt}"
            command = [
                "codex",
                "exec",
                *model_argv(host_model),
                "--json",
                "--sandbox",
                "workspace-write",
                adapter,
            ]
        elif options.surface == "agent":
            command = [
                "claude",
                *model_argv(host_model),
                "--agent",
                "orchestrator",
                "-p",
                "--output-format",
                "stream-json",
                "--verbose",
                "--permission-mode",
                "dontAsk",
                CLAUDE_LIVE_ALLOWED_TOOLS_ARG,
                prompt,
            ]
        else:
            command = [
                "claude",
                *model_argv(host_model),
                "-p",
                "--output-format",
                "stream-json",
                "--verbose",
                "--permission-mode",
                "dontAsk",
                CLAUDE_LIVE_ALLOWED_TOOLS_ARG,
                f"/pl-orchestrator {plan_id}\n\n{prompt}",
            ]
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
        extract_host_transcript(raw, options.vendor, artifact_dir)
        collect_live_after(artifact_dir, repo, env, plan_id)
        grade_live_artifacts(case, artifact_dir, options)
        write_grade(artifact_dir, "pass", case_id, options)
        pass_line(
            f"{case_id}: {options.vendor}/{options.surface} live phase3-preview"
        )
        finish_artifacts(artifact_dir, options)
    except (EvalFailure, EvalBlocked) as exc:
        if exc.artifacts is None:
            exc.artifacts = artifact_dir
        raise


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
        arena.stage_vendor_config(env, options.vendor)
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
    ordered by (ts, seq) -- the file-per-call layout C2 requires so that
    concurrent wrapper lanes cannot interleave a record. A leftover
    `.tmp-observed-*` file from an interrupted write is ignored; the
    wrapper only ever renames a fully-written file into place."""
    if not observed_dir.is_dir():
        return []
    records: list[dict[str, Any]] = []
    for path in sorted(observed_dir.glob("*.json")):
        if path.name.startswith(".tmp-"):
            continue
        value = read_json(path)
        if isinstance(value, dict):
            records.append(value)
    records.sort(
        key=lambda record: (float(record.get("ts", 0)), int(record.get("seq", 0)))
    )
    return records


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
    first violation found in (ts, seq) order.

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
    ordered = sorted(
        observed_records,
        key=lambda record: (float(record.get("ts", 0)), int(record.get("seq", 0))),
    )
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
    internally ordered, so a stable sort by timestamp merges them
    correctly, including a completion recorded before the reviewer
    boundary event.
    """
    merged: list[tuple[float, dict[str, Any]]] = []
    for record in read_observed_records(observed_dir):
        name = observed_event_name(record)
        if name is not None:
            merged.append(
                (
                    float(record.get("ts", 0)),
                    {
                        "event": name,
                        "source": "planar-agent",
                        "ts": record.get("ts"),
                        "seq": record.get("seq"),
                    },
                )
            )
    for index, event in enumerate(event_objects(boundary_events_path)):
        ts = event.get("ts")
        sort_key = float(ts) if ts is not None else float(index)
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
    # recheck is a no-op for those rather than a new hard requirement.
    observed_dir = artifact_dir / "repo" / ".eval" / "observed"
    if observed_dir.is_dir():
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
    post_state = case.get("expected", {}).get("post_state", {})
    task = read_json(artifact_dir / "task.after.json")
    expected_status = post_state.get("task_status")
    if expected_status and task.get("status") != expected_status:
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
    expected_value = post_state.get("file_value")
    if expected_value and actual_value != expected_value:
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
    case_path: Path, case: dict[str, Any], options: Options
) -> None:
    context = prepare_lifecycle_fixture(case_path, case, options, "fixture")
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


def append_file(target: Path, source: Path) -> None:
    with target.open("ab") as output, source.open("rb") as incoming:
        output.write(b"\n")
        shutil.copyfileobj(incoming, output)


def run_lifecycle_host(
    case_path: Path, case: dict[str, Any], options: Options
) -> None:
    context = prepare_lifecycle_fixture(
        case_path, case, options, f"{options.vendor}-{options.surface}"
    )
    artifacts = context.artifacts
    raw = artifacts / "transcript.jsonl"
    timeout = int(case["lifecycle"].get("timeout_seconds", 600))
    prompt = case["lifecycle"]["prompt"].replace("{{PLAN_ID}}", context.plan_id)
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
        if options.vendor == "codex":
            command = [
                "codex",
                "exec",
                "--json",
                "--sandbox",
                "workspace-write",
                "-c",
                f"developer_instructions={encoded_instructions}",
                "Execute this lifecycle directly as the orchestrator. "
                f"Do not read or invoke any skill. {prompt}",
            ]
        else:
            command = [
                "claude",
                "--agent",
                "orchestrator",
                "-p",
                "--output-format",
                "stream-json",
                "--verbose",
                "--permission-mode",
                "dontAsk",
                CLAUDE_LIVE_ALLOWED_TOOLS_ARG,
                prompt,
            ]
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
            if options.vendor == "codex":
                command = [
                    "codex",
                    "exec",
                    "resume",
                    "--json",
                    "-c",
                    f"developer_instructions={encoded_instructions}",
                    session_id,
                    response,
                ]
            else:
                command = [
                    "claude",
                    "--resume",
                    session_id,
                    "-p",
                    "--output-format",
                    "stream-json",
                    "--verbose",
                    "--permission-mode",
                    "dontAsk",
                    CLAUDE_LIVE_ALLOWED_TOOLS_ARG,
                    response,
                ]
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
            task_status = run_json(
                ["planar", "task", "show", context.task_id, "--json"],
                cwd=context.repo,
                env=context.env,
            ).get("status")
        extract_host_transcript(raw, options.vendor, artifacts)
        collect_lifecycle_artifacts(context, options)
        grade_lifecycle_artifacts(case, artifacts, options)
        write_grade(artifacts, "pass", case["id"], options)
        pass_line(
            f"{case['id']}: {options.vendor}/{options.surface} controlled lifecycle"
        )
        finish_artifacts(artifacts, options)
    except (EvalFailure, EvalBlocked) as exc:
        if exc.artifacts is None:
            exc.artifacts = artifacts
        raise


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


def regrade_artifacts(
    artifact_dir: Path, cases: list[tuple[Path, dict[str, Any]]]
) -> None:
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
    if options.mode == "live":
        grade_live_artifacts(case, artifact_dir, options)
    elif options.mode in {"lifecycle", "lifecycle-fixture"}:
        grade_lifecycle_artifacts(case, artifact_dir, options)
    else:
        raise EvalFailure(f"artifact mode is not regradable: {options.mode}")
    write_grade(artifact_dir, "pass", case_id, options)
    pass_line(f"{case_id}: retained {options.mode} artifacts regraded")
    print(f"artifacts: {artifact_dir}")


def parse_args(argv: Sequence[str]) -> tuple[Options, bool, Path | None]:
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
    args = parser.parse_args(argv)
    if args.live:
        mode = "live"
    elif args.lifecycle:
        mode = "lifecycle"
    elif args.lifecycle_fixture_only:
        mode = "lifecycle-fixture"
    else:
        mode = "contract"
    options = Options(
        mode=mode,
        vendor=args.vendor,
        surface=args.surface,
        case_filter=args.case_filter,
        results_dir=args.results.resolve() if args.results else None,
        keep=args.keep,
    )
    return options, args.list, args.grade_artifacts


def main(argv: Sequence[str] | None = None) -> int:
    options, list_only, regrade_path = parse_args(argv or sys.argv[1:])
    cases = load_cases()
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
        for path, case in selected:
            if "contract" in case["tiers"]:
                grade_contract(path, case)
        pass_line(f"{len(selected)} orchestrator case definitions and deterministic contracts")
        return 0
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
        for path, case in runnable:
            run_phase3_preview(path, case, options)
        pass_line(f"{len(runnable)} live orchestrator cases")
        return 0
    if options.mode == "lifecycle-fixture":
        require_commands(["planar", "planar-agent", "planar-watch", "git"])
        runnable = [entry for entry in selected if "lifecycle" in entry[1]["tiers"]]
        if not runnable:
            raise EvalFailure("no selected cases declare the lifecycle tier")
        for path, case in runnable:
            run_lifecycle_fixture_replay(path, case, options)
        pass_line(f"{len(runnable)} controlled lifecycle fixture replays")
        return 0
    if options.vendor not in {"codex", "claude"}:
        raise EvalFailure("--vendor must be codex or claude")
    if options.surface != "agent":
        raise EvalFailure("lifecycle mode currently supports --surface agent")
    require_commands(["planar", "planar-agent", "planar-watch", "git", options.vendor])
    require_current_installed_projection()
    runnable = [entry for entry in selected if "lifecycle" in entry[1]["tiers"]]
    if not runnable:
        raise EvalFailure("no selected cases declare the lifecycle tier")
    for path, case in runnable:
        run_lifecycle_host(path, case, options)
    pass_line(f"{len(runnable)} live lifecycle orchestrator cases")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EvalBlocked as exc:
        print(f"BLOCKED: {exc}", file=sys.stderr)
        if exc.artifacts:
            print(f"artifacts retained: {exc.artifacts}", file=sys.stderr)
        raise SystemExit(75)
    except EvalFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        if exc.artifacts:
            print(f"artifacts retained: {exc.artifacts}", file=sys.stderr)
        raise SystemExit(1)
