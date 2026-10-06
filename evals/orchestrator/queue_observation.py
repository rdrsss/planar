"""Finite, read-only observation of a detached Planar queue ticket.

The detached submitter owns the job. This module owns only its short-lived
wait/status subprocesses and never retries a submission.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


CLEANUP_SECONDS = 0.5
POLL_SECONDS = 1.0
_capabilities: dict[str, str] = {}


@dataclass(frozen=True)
class Observation:
    seq: int
    observed_seq: int | None
    wait_reason: str
    result_exit_code: int | None
    status: dict[str, Any] | None
    error: dict[str, str] | None
    mode: str


class _Interrupted(Exception):
    def __init__(self, signum: int):
        self.signum = signum


def _interrupt(signum: int, _frame: Any) -> None:
    raise _Interrupted(signum)


class _Signals:
    def __enter__(self) -> None:
        self.previous: dict[int, Any] = {}
        try:
            for signum in (signal.SIGINT, signal.SIGTERM):
                previous = signal.getsignal(signum)
                if previous != signal.SIG_IGN:
                    signal.signal(signum, _interrupt)
                    self.previous[signum] = previous
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_exc: Any) -> None:
        for signum, previous in self.previous.items():
            signal.signal(signum, previous)


def _stop_helper(process: subprocess.Popen[str], allowance: float) -> None:
    """Stop and reap only the process group created for this helper."""
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.communicate(timeout=max(0.001, allowance / 2))
    except subprocess.TimeoutExpired:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.communicate(timeout=max(0.001, allowance / 2))
    finally:
        for pipe in (process.stdout, process.stderr):
            if pipe is not None:
                pipe.close()


def _helper(
    args: Sequence[str], cwd: Path, env: dict[str, str], deadline: float
) -> subprocess.CompletedProcess[str]:
    remaining = deadline - time.monotonic()
    if remaining <= CLEANUP_SECONDS + 0.01:
        raise TimeoutError("observation budget exhausted before helper launch")
    process = subprocess.Popen(
        list(args), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(
            timeout=max(0.001, deadline - time.monotonic() - CLEANUP_SECONDS)
        )
        return subprocess.CompletedProcess(list(args), process.returncode, stdout, stderr)
    except subprocess.TimeoutExpired as exc:
        _stop_helper(process, min(CLEANUP_SECONDS, max(0.001, deadline - time.monotonic())))
        raise TimeoutError("queue helper exceeded the observation budget") from exc
    except BaseException:
        _stop_helper(process, min(CLEANUP_SECONDS, max(0.001, deadline - time.monotonic())))
        raise
    finally:
        for pipe in (process.stdout, process.stderr):
            if pipe is not None:
                pipe.close()


def _json_result(result: subprocess.CompletedProcess[str]) -> dict[str, Any]:
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ValueError(f"queue helper returned invalid JSON (exit {result.returncode})") from exc
    if not isinstance(value, dict):
        raise ValueError("queue helper returned a non-object JSON result")
    return value


def discover(
    agent: str, cwd: Path, env: dict[str, str], deadline: float
) -> str:
    """Return native, compatibility or absent; a refusal raises immediately."""
    if agent in _capabilities:
        return _capabilities[agent]
    catalog = _helper([agent, "schema", "--compact"], cwd, env, deadline)
    commands = _json_result(catalog).get("commands") if catalog.returncode == 0 else None
    if not isinstance(commands, list):
        raise ValueError("cannot discover queue capability from command catalog")
    names = {item.get("command") for item in commands if isinstance(item, dict)}
    if "planar-agent queue run" not in names:
        mode = "absent"
    else:
        rule = _helper([agent, "queue", "rule"], cwd, env, deadline)
        if rule.returncode != 0:
            raise ValueError(f"queue refused capability check (exit {rule.returncode}): {rule.stderr.strip()}")
        mode = "native" if "planar-agent queue wait" in names else "compatibility"
    _capabilities[agent] = mode
    return mode


def _completed(status: dict[str, Any]) -> int | None:
    outcome = status.get("outcome")
    if outcome in {"exited", "not_started"}:
        code = status.get("exit_code")
        if type(code) is int and 0 <= code <= 255 and (outcome != "not_started" or code in {126, 127}):
            return code
    if outcome == "signaled":
        signum = status.get("signal")
        if type(signum) is int and 1 <= signum <= 127:
            return 128 + signum
    if outcome == "timeout":
        return 124
    if outcome in {"cancelled", "wait_timeout"}:
        return 125
    return None


def observe(
    seq: int, *, agent: str, cwd: Path, env: dict[str, str],
    timeout_seconds: float = 20.0,
) -> Observation:
    """Take one finite slice. The caller explicitly decides whether to resume."""
    if type(seq) is not int or seq <= 0 or not 0 < timeout_seconds <= 86400:
        raise ValueError("positive ticket and finite observation timeout required")
    deadline = time.monotonic() + timeout_seconds
    mode = "undiscovered"
    latest: dict[str, Any] | None = None
    observed_seq: int | None = None

    def finish(reason: str, code: int | None = None, error: str | None = None) -> Observation:
        return Observation(seq, observed_seq, reason, code, latest,
                           {"message": error} if error else None, mode)

    try:
        with _Signals():
            mode = discover(agent, cwd, env, deadline)
            if mode == "absent":
                return finish("error", 125, "queue verb is absent")
            if mode == "native":
                remaining_ms = int((deadline - time.monotonic() - CLEANUP_SECONDS - 0.05) * 1000)
                if remaining_ms <= 0:
                    return finish("timed_out", 124)
                result = _helper(
                    [agent, "queue", "wait", str(seq), "--timeout", f"{remaining_ms}ms", "--json"],
                    cwd, env, deadline,
                )
                value = _json_result(result)
                if value.get("seq") != seq or value.get("wait_reason") not in {
                    "completed", "timed_out", "interrupted", "stalled", "history_unavailable", "error"
                }:
                    return finish("error", 125, "invalid native wait envelope")
                latest = value.get("status") if isinstance(value.get("status"), dict) else None
                observed_seq = value.get("observed_seq") if type(value.get("observed_seq")) is int else None
                reason = value["wait_reason"]
                code = value.get("result_exit_code")
                if type(code) is not int or code != result.returncode:
                    return finish("error", 125, "native wait exit/envelope mismatch")
                if reason == "completed" and (
                    latest is None or latest.get("state") != "ended" or _completed(latest) != code
                ):
                    return finish("error", 125, "native wait lacks authoritative completed outcome")
                return Observation(seq, observed_seq, reason, code, latest,
                                   value.get("error") if isinstance(value.get("error"), dict) else None, mode)

            dead_since: float | None = None
            dead_seq: int | None = None
            while True:
                if deadline - time.monotonic() <= CLEANUP_SECONDS + 0.01:
                    return finish("timed_out", 124)
                result = _helper([agent, "queue", "status", str(seq), "--json"], cwd, env, deadline)
                value = _json_result(result)
                if result.returncode != 0:
                    error = value.get("error")
                    if result.returncode == 1 and isinstance(error, dict) and error.get("tag") == "not_found":
                        return finish("history_unavailable", 1, "ticket history unavailable")
                    return finish("error", 125, str(error or result.stderr.strip() or "queue status refused"))
                if value.get("state") not in {"waiting", "running", "terminating", "ended"}:
                    return finish("error", 125, "invalid queue status state")
                latest = value
                observed_seq = value.get("seq") if type(value.get("seq")) is int else None
                if value["state"] == "ended":
                    if value.get("outcome") != "abandoned":
                        code = _completed(value)
                        return finish("completed", code) if code is not None else finish("error", 125, "invalid queue history")
                    dead_since = None
                elif value.get("live") is False:
                    if dead_seq != observed_seq or dead_since is None:
                        dead_seq, dead_since = observed_seq, time.monotonic()
                    elif time.monotonic() - dead_since >= 1.0:
                        return finish("stalled", 125, "active entry is confirmed dead")
                else:
                    dead_since = None
                time.sleep(min(POLL_SECONDS, max(0.0, deadline - time.monotonic() - CLEANUP_SECONDS)))
    except _Interrupted as exc:
        return finish("interrupted", 128 + exc.signum)
    except TimeoutError:
        return finish("timed_out", 124)
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        return finish("error", 125, str(exc))


def submit(
    command: Sequence[str], *, agent: str, cwd: Path, env: dict[str, str],
    vendor: str = "codex", role: str = "orchestrator", timeout_seconds: float = 60.0,
) -> tuple[int, Path]:
    """Issue one ticket; refusal cannot be mistaken for an issued sequence."""
    deadline = time.monotonic() + timeout_seconds
    mode = discover(agent, cwd, env, deadline)
    if mode == "absent":
        raise ValueError("queue verb is absent")
    result = _helper(
        [agent, "queue", "run", "--detach", "--vendor", vendor, "--role", role,
         "--timeout", "5m", "--", *command], cwd, env, deadline,
    )
    if result.returncode != 0:
        raise ValueError(f"queue submission refused (exit {result.returncode}): {result.stderr.strip()}")
    lines = result.stdout.splitlines()
    if len(lines) != 2 or not lines[0].isdecimal() or int(lines[0]) <= 0:
        raise ValueError("queue submission omitted its ticket and log path")
    return int(lines[0]), Path(lines[1])
