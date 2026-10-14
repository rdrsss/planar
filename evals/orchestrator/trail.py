#!/usr/bin/env python3
"""Grade the recorded trail of any orchestrator through `planar health conformance`.

The orchestrator suite used to grade one prompt: the stock orchestrator agent's
text, and the event order the stock lifecycle produced. This module grades what
any orchestrator leaves behind instead. A *trail* is the claims, actions,
reviews, queued runs and task states in a Planar database. The grader copies the
database to scratch, runs `planar health conformance <plan> --json` over the
copy, and judges the checker's own report. It never reads which prompt, vendor,
role convention or session produced the trail; a trail is graded the same
whoever wrote it.

Two things are built on that one function:

- `grade_database` / `grade_report` grade a trail that already exists, for
  example the scratch database a lifecycle host run left in its arena.
- `build_trail` materialises a *recorded trail* from a JSON specification
  (`evals/orchestrator/trails/*.json`), so the checker's contract is exercised
  by trails shaped like different orchestrators, each with its expected verdict.

Safety: every subprocess runs with `HOME`, `PLANAR_DB`, `PLANAR_HOME`,
`PLANAR_LOCAL_HOME`, `PLANAR_WORKBENCH_ROOT` and `PLANAR_CONFIG_PATH` pointed
into a throwaway directory, and a trail database is only ever read through
SQLite's backup API into that directory, so grading cannot write to the source
database or reach `~/.planar`. Standard library only.
"""

from __future__ import annotations

import json
import os
import shutil
import sqlite3
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Sequence

ROOT = Path(__file__).resolve().parents[2]
TRAILS_DIR = ROOT / "evals" / "orchestrator" / "trails"

# The verb this module drives. A planar that lacks it cannot grade a trail.
CONFORMANCE_VERB = ("health", "conformance")
RULES = {
    1: "completed-claim",
    2: "terminal-verb",
    3: "coder-model",
    4: "parent-action",
    5: "queued-gate",
    6: "independent-review",
}
ENTITY_KINDS = ("task", "claim")


class TrailError(RuntimeError):
    """A trail could not be built, graded or read."""


class TrailUnavailable(TrailError):
    """No `planar` that supports `health conformance` could be found."""


# --- locating a planar that can grade -------------------------------------


_SUPPORT_CACHE: dict[str, bool] = {}


def scratch_env(directory: Path, base: Mapping[str, str] | None = None) -> dict[str, str]:
    """An environment whose every Planar path lies under `directory`.

    Only `PATH` is carried over from `base` (default `os.environ`): the binary
    must be found, and nothing else of the operator's environment may leak in.
    """
    source = os.environ if base is None else base
    for name in ("home", "planar-home", "local-home", "workbench", "tmp"):
        (directory / name).mkdir(parents=True, exist_ok=True)
    return {
        "PATH": source.get("PATH", os.defpath),
        "HOME": str(directory / "home"),
        "PLANAR_DB": str(directory / "planar.db"),
        "PLANAR_HOME": str(directory / "planar-home"),
        "PLANAR_LOCAL_HOME": str(directory / "local-home"),
        "PLANAR_WORKBENCH_ROOT": str(directory / "workbench"),
        "PLANAR_CONFIG_PATH": str(directory / "config.toml"),
        "TMPDIR": str(directory / "tmp"),
    }


def assert_scratch(env: Mapping[str, str], directory: Path) -> None:
    """Fail closed unless every Planar path in `env` resolves inside `directory`."""
    root = directory.resolve()
    for name in (
        "HOME",
        "PLANAR_DB",
        "PLANAR_HOME",
        "PLANAR_LOCAL_HOME",
        "PLANAR_WORKBENCH_ROOT",
        "PLANAR_CONFIG_PATH",
    ):
        value = env.get(name)
        if not value or not Path(value).resolve().is_relative_to(root):
            raise TrailError(f"{name} does not resolve inside the scratch directory: {value!r}")


def supports_conformance(planar: Path) -> bool:
    """Whether `planar` has `health conformance` (probed once per path)."""
    key = str(planar)
    if key not in _SUPPORT_CACHE:
        with tempfile.TemporaryDirectory(prefix="planar-trail-probe-") as tmp:
            scratch = Path(tmp)
            env = scratch_env(scratch)
            try:
                result = subprocess.run(
                    [key, *CONFORMANCE_VERB, "--help"],
                    capture_output=True,
                    text=True,
                    env=env,
                    cwd=scratch,
                    timeout=60,
                    check=False,
                )
            except OSError:
                _SUPPORT_CACHE[key] = False
            else:
                _SUPPORT_CACHE[key] = result.returncode == 0 and "conformance" in result.stdout
    return _SUPPORT_CACHE[key]


def resolve_planar(
    root: Path = ROOT, env: Mapping[str, str] | None = None
) -> Path:
    """The `planar` to grade with.

    Order: `PLANAR_EVAL_PLANAR` (explicit, and an error when unusable), the
    checkout's `build/debug/bin/planar`, its `bin/planar`, then `planar` on
    `PATH`. The first that supports `health conformance` wins, so a freshly
    built checkout grades its own trails even when an older release is
    installed.
    """
    source = os.environ if env is None else env
    explicit = source.get("PLANAR_EVAL_PLANAR")
    if explicit:
        path = Path(explicit)
        if not path.is_file() or not supports_conformance(path):
            raise TrailUnavailable(
                f"PLANAR_EVAL_PLANAR={explicit} is not a planar with `health conformance`"
            )
        return path
    candidates = [root / "build" / "debug" / "bin" / "planar", root / "bin" / "planar"]
    on_path = shutil.which("planar", path=source.get("PATH"))
    if on_path:
        candidates.append(Path(on_path))
    tried: list[str] = []
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            if supports_conformance(candidate):
                return candidate
            tried.append(f"{candidate} (no `health conformance`)")
        else:
            tried.append(f"{candidate} (not found)")
    raise TrailUnavailable(
        "no planar with `health conformance` was found; tried: "
        + "; ".join(tried)
        + ". Build the checkout (cmake --build build/debug) or set PLANAR_EVAL_PLANAR."
    )


# --- the checker's report --------------------------------------------------


@dataclass(frozen=True)
class TrailGrade:
    """The checker's verdict on one trail."""

    exit_code: int
    report: dict[str, Any]

    @property
    def conforms(self) -> bool:
        return bool(self.report["conforms"])

    @property
    def violations(self) -> list[dict[str, Any]]:
        return list(self.report["violations"])

    @property
    def overrides(self) -> list[dict[str, Any]]:
        return list(self.report["overrides"])

    def violation_keys(self) -> list[tuple[int, str, int]]:
        """`(rule, entity_kind, entity_id)` for every violation, sorted."""
        return sorted(
            (v["rule"], v["entity_kind"], v["entity_id"]) for v in self.violations
        )


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise TrailError(f"conformance report: {message}")


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def parse_report(text: str, exit_code: int) -> TrailGrade:
    """Parse and validate `planar health conformance --json` output.

    The shape is the checker's contract: a missing key, a wrong type, a rule
    outside 1-6 or a `conforms` that disagrees with the violation list or the
    exit code is a `TrailError`, never silently accepted.
    """
    try:
        report = json.loads(text)
    except json.JSONDecodeError as exc:
        raise TrailError(f"conformance report is not JSON: {exc}") from exc
    _require(isinstance(report, dict), "top level is not an object")
    for key in (
        "plan_id",
        "plans",
        "conforms",
        "floor",
        "graded",
        "violations",
        "overrides",
        "policy_changed",
        "before_floor",
    ):
        _require(key in report, f"missing key {key!r}")
    _require(_is_int(report["plan_id"]) and _is_int(report["plans"]), "plan_id/plans must be integers")
    _require(isinstance(report["conforms"], bool), "conforms must be a boolean")
    floor = report["floor"]
    _require(isinstance(floor, dict), "floor must be an object")
    _require(_is_int(floor.get("claim_id")), "floor.claim_id must be an integer")
    _require(floor.get("origin") in ("migration", "digest", "explicit"), "floor.origin is unknown")
    _require(floor.get("cut_at") is None or isinstance(floor["cut_at"], str), "floor.cut_at must be text or null")
    for section, keys in (("graded", ("claims", "done_tasks")), ("before_floor", ("claims", "done_tasks"))):
        _require(isinstance(report[section], dict), f"{section} must be an object")
        for key in keys:
            _require(_is_int(report[section].get(key)), f"{section}.{key} must be an integer")
    _require(isinstance(report["violations"], list), "violations must be an array")
    for item in report["violations"]:
        _require(isinstance(item, dict), "a violation must be an object")
        _require(item.get("rule") in RULES, f"violation rule out of range: {item.get('rule')!r}")
        _require(item.get("name") == RULES[item["rule"]], f"violation name does not match rule {item['rule']}")
        _require(item.get("entity_kind") in ENTITY_KINDS, "violation entity_kind must be task or claim")
        _require(_is_int(item.get("entity_id")), "violation entity_id must be an integer")
        _require(isinstance(item.get("reason"), str) and item["reason"], "violation needs a reason")
    _require(isinstance(report["overrides"], list), "overrides must be an array")
    for item in report["overrides"]:
        _require(isinstance(item, dict) and _is_int(item.get("task_id")), "an override needs a task_id")
    _require(isinstance(report["policy_changed"], list), "policy_changed must be an array")
    _require(
        report["conforms"] == (not report["violations"]),
        "conforms disagrees with the violation list",
    )
    _require(
        exit_code == (0 if report["conforms"] else 1),
        f"exit code {exit_code} disagrees with conforms={report['conforms']}",
    )
    return TrailGrade(exit_code=exit_code, report=report)


def describe(grade: TrailGrade) -> str:
    """The violations and overrides, one line each, for a failure message."""
    lines = [
        f"plan {grade.report['plan_id']}: {len(grade.violations)} violation(s), "
        f"{len(grade.overrides)} override(s), floor claim {grade.report['floor']['claim_id']}"
    ]
    for item in grade.violations:
        lines.append(
            f"  rule {item['rule']} {item['name']}: {item['entity_kind']} {item['entity_id']}: {item['reason']}"
        )
    for item in grade.overrides:
        lines.append(f"  override: task {item['task_id']}: {item['reason']}")
    return "\n".join(lines)


# --- grading a database ----------------------------------------------------


def snapshot_database(source: Path, destination: Path) -> None:
    """Copy `source` to `destination` through SQLite's backup API, read-only on the source."""
    if not source.is_file():
        raise TrailError(f"trail database not found: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    reader = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    try:
        writer = sqlite3.connect(destination)
        try:
            reader.backup(writer)
        finally:
            writer.close()
    finally:
        reader.close()


def policy_toml(policy: Mapping[str, Any] | None) -> str:
    """A `config.toml` with a `[policy]` table for `policy` (empty for none)."""
    if not policy:
        return ""
    lines = ["[policy]"]
    for key, value in policy.items():
        if isinstance(value, bool):
            lines.append(f"{key} = {'true' if value else 'false'}")
        elif isinstance(value, list):
            lines.append(f"{key} = {json.dumps(value)}")
        else:
            raise TrailError(f"policy {key!r} must be a boolean or a list of strings")
    return "\n".join(lines) + "\n"


def run_conformance(
    database: Path,
    plan_id: int | str,
    *,
    floor: int | None = None,
    config_text: str = "",
    scope: str | None = None,
    planar: Path | None = None,
) -> TrailGrade:
    """Grade the trail of `plan_id` in `database` and return the checker's report.

    The database is copied to a throwaway directory first, so the source is
    never opened for writing and `planar` (which migrates a database it opens)
    only ever touches the copy. `config_text` is the policy the trail is graded
    under; it is written to the throwaway `config.toml`.
    """
    binary = planar or resolve_planar()
    with tempfile.TemporaryDirectory(prefix="planar-trail-") as tmp:
        scratch = Path(tmp)
        env = scratch_env(scratch)
        assert_scratch(env, scratch)
        snapshot_database(database, Path(env["PLANAR_DB"]))
        Path(env["PLANAR_CONFIG_PATH"]).write_text(config_text, encoding="utf-8")
        argv = [str(binary), *CONFORMANCE_VERB, str(plan_id), "--json"]
        if floor is not None:
            argv += ["--floor", str(floor)]
        if scope is not None:
            argv += ["--scope", scope]
        result = subprocess.run(
            argv, capture_output=True, text=True, env=env, cwd=scratch, timeout=300, check=False
        )
    # Exit 1 means both "the trail broke a rule" and "the plan, scope or policy could not be read". Only the
    # presence of the JSON report tells them apart, so a refusal is never read as a verdict.
    if result.returncode not in (0, 1) or not result.stdout.lstrip().startswith("{"):
        raise TrailError(
            f"{' '.join(argv[1:])} produced no report (exit {result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip() or 'no output'}"
        )
    return parse_report(result.stdout, result.returncode)


def grade_database(
    database: Path,
    plan_id: int | str,
    *,
    floor: int | None = 1,
    config_text: str = "",
    planar: Path | None = None,
) -> TrailGrade:
    """`run_conformance` with the whole trail graded by default (`floor` 1)."""
    return run_conformance(database, plan_id, floor=floor, config_text=config_text, planar=planar)


# --- recorded trails -------------------------------------------------------

_SPEC_KEYS = {
    "schema_version",
    "id",
    "description",
    "style",
    "policy",
    "floor",
    "child_plan",
    "tasks",
    "claims",
    "expected",
}
_TASK_KEYS = {"title", "final", "plan"}
_TASK_FINALS = ("todo", "doing", "done", "done-forced")
_CLAIM_KEYS = {
    "task",
    "status",
    "role",
    "model",
    "vendor",
    "session",
    "parent_action",
    "self_nested_action",
    "lease",
    "repo_root",
    "reviews",
    "gate_runs",
}
_CLAIM_STATUSES = ("active", "released", "completed", "aborted", "stale")
_REVIEW_KEYS = {"session", "verdict"}
_GATE_KEYS = {"argv", "cwd", "outcome", "exit_code"}
_EXPECTED_KEYS = {"conforms", "violations", "overrides", "before_floor"}
_POLICY_KEYS = {
    "review_required",
    "task_gate",
    "require_model",
    "confirm_before",
    "require_scenario_coverage",
    "use_queue",
}


def _check_keys(where: str, value: Mapping[str, Any], allowed: set[str]) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise TrailError(f"{where}: unknown key(s) {', '.join(unknown)}")


def validate_spec(spec: Any) -> None:
    """Reject a trail specification that is malformed, with the offending key named."""
    if not isinstance(spec, dict):
        raise TrailError("trail specification must be an object")
    _check_keys("trail", spec, _SPEC_KEYS)
    if spec.get("schema_version") != 1:
        raise TrailError("trail: schema_version must be 1")
    for key in ("id", "description", "tasks", "claims", "expected"):
        if key not in spec:
            raise TrailError(f"trail: missing {key!r}")
    if not isinstance(spec["id"], str) or not spec["id"]:
        raise TrailError("trail: id must be non-empty text")
    _check_keys(f"{spec['id']} policy", spec.get("policy") or {}, _POLICY_KEYS)
    tasks = spec["tasks"]
    if not isinstance(tasks, list) or not tasks:
        raise TrailError(f"{spec['id']}: tasks must be a non-empty array")
    for index, task in enumerate(tasks, 1):
        _check_keys(f"{spec['id']} task {index}", task, _TASK_KEYS)
        if task.get("final") not in _TASK_FINALS:
            raise TrailError(f"{spec['id']} task {index}: final must be one of {_TASK_FINALS}")
        if task.get("plan", "anchor") not in ("anchor", "child"):
            raise TrailError(f"{spec['id']} task {index}: plan must be anchor or child")
        if task.get("plan") == "child" and not spec.get("child_plan"):
            raise TrailError(f"{spec['id']} task {index}: plan child needs child_plan true")
    for index, claim in enumerate(spec["claims"], 1):
        where = f"{spec['id']} claim {index}"
        _check_keys(where, claim, _CLAIM_KEYS)
        task = claim.get("task")
        if not _is_int(task) or not 1 <= task <= len(tasks):
            raise TrailError(f"{where}: task must be a 1-based index into tasks")
        if claim.get("status") not in _CLAIM_STATUSES:
            raise TrailError(f"{where}: status must be one of {_CLAIM_STATUSES}")
        if claim.get("lease", "live") not in ("live", "expired"):
            raise TrailError(f"{where}: lease must be live or expired")
        for review in claim.get("reviews", []):
            _check_keys(f"{where} review", review, _REVIEW_KEYS)
            if review.get("verdict") not in ("approve", "request-changes", None):
                raise TrailError(f"{where}: review verdict must be approve, request-changes or null")
        for run in claim.get("gate_runs", []):
            _check_keys(f"{where} gate run", run, _GATE_KEYS)
            if not isinstance(run.get("argv"), list):
                raise TrailError(f"{where}: a gate run needs an argv array")
    expected = spec["expected"]
    _check_keys(f"{spec['id']} expected", expected, _EXPECTED_KEYS)
    if not isinstance(expected.get("conforms"), bool):
        raise TrailError(f"{spec['id']}: expected.conforms must be a boolean")
    for item in expected.get("violations", []):
        _check_keys(f"{spec['id']} expected violation", item, {"rule", "entity"})
        if item.get("rule") not in RULES or not isinstance(item.get("entity"), str) or ":" not in item["entity"]:
            raise TrailError(f"{spec['id']}: an expected violation needs a rule 1-6 and an entity like task:2")
    floor = spec.get("floor", "all")
    if not (floor in ("all", "default") or (isinstance(floor, dict) and set(floor) <= {"claim_id", "migration_applied_at"})):
        raise TrailError(f"{spec['id']}: floor must be all, default, or an object with claim_id or migration_applied_at")


def load_trail(path: Path) -> dict[str, Any]:
    """Read and validate one trail specification."""
    spec = json.loads(path.read_text(encoding="utf-8"))
    validate_spec(spec)
    if spec["id"] != path.stem:
        raise TrailError(f"{path.name}: id {spec['id']!r} must equal the file name")
    return spec


def load_trails(directory: Path = TRAILS_DIR) -> list[tuple[Path, dict[str, Any]]]:
    """Every trail specification under `directory`, ordered by file name."""
    return [(path, load_trail(path)) for path in sorted(directory.glob("*.json"))]


@dataclass
class BuiltTrail:
    """A recorded trail materialised as a database."""

    database: Path
    plan_id: int
    config_text: str
    floor: int | None
    task_ids: list[int] = field(default_factory=list)


def _run(planar: Path, argv: Sequence[str], cwd: Path, env: Mapping[str, str]) -> str:
    result = subprocess.run(
        [str(planar), *argv], capture_output=True, text=True, env=dict(env), cwd=cwd, timeout=120, check=False
    )
    if result.returncode != 0:
        raise TrailError(f"planar {' '.join(argv)} exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


_ROLE_ACTION_KIND = {"coder": "coder", "reviewer": "reviewer", "test-coder": "test_coder", "planner": "planner"}


def build_trail(spec: Mapping[str, Any], directory: Path, planar: Path | None = None) -> BuiltTrail:
    """Materialise `spec` as a database under `directory`.

    The plan and its tasks are made with the real verbs, so the audit rows are
    the ones an operator's own commands leave. Claims, actions, reviews and
    queued runs are raw rows, because only another binary writes them: they are
    the *recorded* part of the trail, in the shapes the verbs record.
    """
    validate_spec(spec)
    binary = planar or resolve_planar()
    env = scratch_env(directory)
    assert_scratch(env, directory)
    repo = directory / "repo"
    repo.mkdir(parents=True, exist_ok=True)
    _run(binary, ["init", "--name", spec["id"], "--json"], repo, env)
    _run(binary, ["plan", "create", spec["id"], "--scope", "global", "--slug", f"trail-{spec['id']}"], repo, env)
    child_plan = None
    if spec.get("child_plan"):
        _run(
            binary,
            ["plan", "create", "child", "--scope", "global", "--slug", f"trail-{spec['id']}-child", "--parent", "1"],
            repo,
            env,
        )
        child_plan = 2
    task_ids: list[int] = []
    for index, task in enumerate(spec["tasks"], 1):
        plan = child_plan if task.get("plan") == "child" else 1
        _run(
            binary,
            ["task", "add", task.get("title", f"task {index}"), "--plan", str(plan), "--scope", "global"],
            repo,
            env,
        )
        task_ids.append(index)
        final = task["final"]
        if final in ("doing", "done"):
            _run(binary, ["task", "update", str(index), "--status", "doing"], repo, env)
        if final == "done":
            _run(binary, ["task", "done", str(index)], repo, env)
        elif final == "done-forced":
            _run(binary, ["task", "done", str(index), "--force"], repo, env)

    database = Path(env["PLANAR_DB"])
    connection = sqlite3.connect(database)
    try:
        connection.execute("pragma foreign_keys = on")
        sessions: dict[str, int] = {}
        orchestrator_action: dict[str, int] = {}

        def session_for(label: str, vendor: str) -> int:
            if label not in sessions:
                cursor = connection.execute("insert into sessions (vendor) values (?)", (vendor,))
                sessions[label] = int(cursor.lastrowid)
            return sessions[label]

        gate_seq = 0
        for number, claim in enumerate(spec["claims"], 1):
            vendor = claim.get("vendor", "claude")
            label = claim.get("session", f"session-{number}")
            session = session_for(label, vendor)
            token = f"{number:032x}"
            status = claim["status"]
            lease = "2000-01-01T00:00:00.000Z" if claim.get("lease") == "expired" else "2999-01-01T00:00:00.000Z"
            connection.execute(
                "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, "
                "role, model, repo_root, lease_expires_at, released_at) "
                "values (?, ?, 'task', ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    token,
                    session,
                    claim["task"],
                    status,
                    vendor,
                    claim.get("role"),
                    claim.get("model"),
                    claim.get("repo_root", "/work/repo"),
                    lease,
                    None if status == "active" else "2026-02-02T00:00:00.000Z",
                ),
            )
            claim_id = int(connection.execute("select max(id) from agent_work_claims").fetchone()[0])
            parent = None
            if claim.get("parent_action"):
                dispatcher = "dispatcher"
                if dispatcher not in orchestrator_action:
                    dispatch_session = session_for(dispatcher, "claude")
                    cursor = connection.execute(
                        "insert into agent_actions (session_id, action_kind, vendor) values (?, 'orchestrator', 'claude')",
                        (dispatch_session,),
                    )
                    orchestrator_action[dispatcher] = int(cursor.lastrowid)
                parent = orchestrator_action[dispatcher]
            kind = _ROLE_ACTION_KIND.get(claim.get("role") or "", "other")
            root = connection.execute(
                "insert into agent_actions (session_id, claim_id, parent_action_id, action_kind, vendor, model) "
                "values (?, ?, ?, ?, ?, ?)",
                (session, claim_id, parent, kind, vendor, claim.get("model")),
            ).lastrowid
            if claim.get("self_nested_action"):
                # What `planar-agent action start --claim` records: a child under the claim's own newest action.
                connection.execute(
                    "insert into agent_actions (session_id, claim_id, parent_action_id, action_kind, vendor) "
                    "values (?, ?, ?, 'tool_call', ?)",
                    (session, claim_id, root, vendor),
                )
            for review in claim.get("reviews", []):
                reviewer = session_for(review["session"], "claude")
                connection.execute(
                    "insert into agent_actions (session_id, claim_id, action_kind, vendor, verdict) "
                    "values (?, ?, 'review', 'claude', ?)",
                    (reviewer, claim_id, review.get("verdict")),
                )
            for run in claim.get("gate_runs", []):
                gate_seq += 1
                connection.execute(
                    "insert into queue_history (seq, outcome, exit_code, nested, cwd, argv, enqueued_at, ended_at, "
                    "waited_ms, claim_token) values (?, ?, ?, 0, ?, ?, 0, 1, 0, ?)",
                    (
                        2_000_000 + gate_seq,
                        run.get("outcome", "exited"),
                        run.get("exit_code", 0),
                        run.get("cwd", claim.get("repo_root", "/work/repo")),
                        json.dumps(run["argv"]),
                        token,
                    ),
                )
        floor_spec = spec.get("floor", "all")
        floor: int | None = 1
        if floor_spec == "default":
            floor = None
        elif isinstance(floor_spec, dict):
            floor = floor_spec.get("claim_id")
            if "migration_applied_at" in floor_spec:
                connection.execute(
                    "update schema_migrations set applied_at = ? where version = 42", (floor_spec["migration_applied_at"],)
                )
        connection.commit()
    finally:
        connection.close()
    return BuiltTrail(
        database=database,
        plan_id=1,
        config_text=policy_toml(spec.get("policy")),
        floor=floor,
        task_ids=task_ids,
    )


def expectation_failures(grade: TrailGrade, expected: Mapping[str, Any]) -> list[str]:
    """Every way `grade` differs from `expected`, as one line each (empty when it matches)."""
    problems: list[str] = []
    if grade.conforms != expected["conforms"]:
        problems.append(f"conforms was {grade.conforms}, expected {expected['conforms']}")
    want = sorted(
        (item["rule"], item["entity"].split(":")[0], int(item["entity"].split(":")[1]))
        for item in expected.get("violations", [])
    )
    have = grade.violation_keys()
    if have != want:
        problems.append(f"violations were {have}, expected {want}")
    if "overrides" in expected:
        override_ids = sorted(item["task_id"] for item in grade.overrides)
        if override_ids != sorted(expected["overrides"]):
            problems.append(f"overrides were {override_ids}, expected {sorted(expected['overrides'])}")
    if "before_floor" in expected:
        actual = grade.report["before_floor"]
        for key, value in expected["before_floor"].items():
            if actual.get(key) != value:
                problems.append(f"before_floor.{key} was {actual.get(key)}, expected {value}")
    return problems


def run_trail(spec: Mapping[str, Any], planar: Path | None = None) -> TrailGrade:
    """Build the recorded trail `spec` describes, grade it, and require its expected verdict.

    Raises `TrailError` naming every difference and printing the checker's own
    description of the trail. Returns the grade when it matched.
    """
    binary = planar or resolve_planar()
    with tempfile.TemporaryDirectory(prefix="planar-trail-build-") as tmp:
        built = build_trail(spec, Path(tmp), binary)
        grade = run_conformance(
            built.database, built.plan_id, floor=built.floor, config_text=built.config_text, planar=binary
        )
    problems = expectation_failures(grade, spec["expected"])
    if problems:
        raise TrailError(f"{spec['id']}: " + "; ".join(problems) + "\n" + describe(grade))
    return grade
