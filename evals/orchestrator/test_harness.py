#!/usr/bin/env python3
from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import arena
import harness


def replace_text(path: Path, old: str, new: str) -> None:
    path.write_text(
        path.read_text(encoding="utf-8").replace(old, new),
        encoding="utf-8",
    )


def live_case() -> dict[str, object]:
    return {
        "id": "semantic-live-probe",
        "tasks": [{"slug": "task-one", "title": "Task one"}],
        "live": {
            "expected_models": {
                "codex": "gpt-5.6-terra",
                "claude": "claude-sonnet-5",
            }
        },
        "expected": {
            "tier": "medium",
            "forbidden_work_type": "architectural",
            "requires_explicit_gate": True,
            "required_preview_terms": ["classic", "isolation"],
        },
    }


def live_options(vendor: str = "codex") -> harness.Options:
    return harness.Options(
        mode="live",
        vendor=vendor,
        surface="agent",
        case_filter=None,
        results_dir=None,
        keep=True,
    )


def write_live_artifacts(
    artifact_dir: Path, options: harness.Options | None = None
) -> None:
    active_options = options or live_options()
    model = live_case()["live"]["expected_models"][active_options.vendor]
    for before, after in (
        ("before.dispatch-state.json", "after.dispatch-state.json"),
        ("before.planar.sql", "after.planar.sql"),
        ("before.git-state", "after.git-state"),
    ):
        harness.write_text(artifact_dir / before, "unchanged\n")
        harness.write_text(artifact_dir / after, "unchanged\n")
    harness.write_json(artifact_dir / "claims.after.json", {"active": []})
    harness.write_text(
        artifact_dir / "final.txt",
        "| task-one | medium | implementation | "
        f"{model} | classic | isolation | confirm |\n",
    )
    harness.write_text(artifact_dir / "transcript.txt", "")
    harness.write_text(artifact_dir / "transcript.jsonl", "{}\n")


def lifecycle_case() -> dict[str, object]:
    return {
        "id": "semantic-lifecycle-probe",
        "expected": {
            "ordered_events": [
                "claim-acquired",
                "coder-finished",
                "review-approved",
                "task-completed",
            ],
            "forbidden_events": ["task-failed"],
            "event_counts": {
                "claim-acquired": 1,
                "coder-finished": 1,
                "review-approved": 1,
                "task-completed": 1,
            },
            "post_state": {
                "task_status": "done",
                "active_claims": 0,
                "file_value": "approved",
            },
        },
    }


def lifecycle_options() -> harness.Options:
    return harness.Options(
        mode="lifecycle-fixture",
        vendor="",
        surface="agent",
        case_filter=None,
        results_dir=None,
        keep=True,
    )


def write_lifecycle_artifacts(artifact_dir: Path) -> None:
    harness.write_json(
        artifact_dir / "events.normalized.json",
        [
            {"event": "claim-acquired"},
            {"event": "coder-finished"},
            {"event": "review-approved"},
            {"event": "task-completed"},
        ],
    )
    harness.write_json(artifact_dir / "task.after.json", {"status": "done"})
    harness.write_json(artifact_dir / "claims.after.json", {"active": []})
    harness.write_text(artifact_dir / "fixture-test.txt", "PASS\n")
    harness.write_json(artifact_dir / "fixture-test.json", {"returncode": 0})
    harness.write_text(artifact_dir / "repo" / "src" / "value.txt", "approved\n")


class GraderPredicateTests(unittest.TestCase):
    def test_forbidden_work_type_ignores_explanatory_prose(self) -> None:
        text = (
            "No unresolved schema, architectural, transaction, or ownership "
            "decision exists, so the task remains medium."
        )
        self.assertFalse(harness.explicit_work_type_assignment(text, "architectural"))

    def test_forbidden_work_type_ignores_slash_separated_prose(self) -> None:
        """A slash-separated enumeration is prose, not an assignment.

        Regression: the orchestrator writes "no citable unresolved
        schema/architectural/engine decision" to explain why it did NOT
        escalate. `/` was accepted as a table delimiter, so that sentence
        graded as an architectural assignment and failed a correct preview.
        """
        text = (
            "all three task bodies are empty, so there is no citable unresolved "
            "schema/architectural/engine decision; all stay at the medium "
            "default with work type feature."
        )
        self.assertFalse(harness.explicit_work_type_assignment(text, "architectural"))

    def test_forbidden_work_type_ignores_override_suggestion(self) -> None:
        """Offering an override is not making the assignment."""
        text = "override it to `large`/`architectural` if you know it carries an open decision."
        self.assertFalse(harness.explicit_work_type_assignment(text, "architectural"))

    def test_forbidden_work_type_detects_table_assignment(self) -> None:
        self.assertTrue(
            harness.explicit_work_type_assignment(
                "| task | `medium` | `architectural` | model |", "architectural"
            )
        )

    def test_cross_host_check_ignores_configured_route_explanation(self) -> None:
        text = (
            "The configured route names claude-sonnet-5, but the active Codex "
            "candidate is gpt-5.6-terra."
        )
        self.assertFalse(harness.cross_host_assignment(text, "codex"))

    def test_cross_host_check_detects_model_assignment(self) -> None:
        self.assertTrue(
            harness.cross_host_assignment(
                "model: `claude-sonnet-5`", "codex"
            )
        )


class ModelInputTests(unittest.TestCase):
    """The two model inputs are independent, validated, and never shell syntax."""

    def test_host_model_and_delegated_candidate_are_independent(self) -> None:
        # Either may be set alone. Deriving one from the other would make a
        # cross-host leak invisible: one field cannot disagree with itself.
        host_only = harness.resolve_model_inputs({"host_model": "claude-opus-5"})
        self.assertEqual(host_only, ("claude-opus-5", None))
        delegated_only = harness.resolve_model_inputs({"delegated_candidate": "gpt-5.6-sol"})
        self.assertEqual(delegated_only, (None, "gpt-5.6-sol"))
        both = harness.resolve_model_inputs(
            {"host_model": "claude-opus-5", "delegated_candidate": "gpt-5.6-sol"}
        )
        self.assertEqual(both, ("claude-opus-5", "gpt-5.6-sol"))
        self.assertEqual(harness.resolve_model_inputs({}), (None, None))

    def test_unsafe_values_are_rejected_with_a_named_error(self) -> None:
        for bad in ("", "has\nnewline", "has\ttab", "two words", "trailing "):
            with self.assertRaises(harness.UnsafeModelValue):
                harness.validate_model_value("host_model", bad)
        with self.assertRaises(harness.UnsafeModelValue):
            harness.validate_model_value("host_model", 5)

    def test_shell_punctuation_survives_as_data(self) -> None:
        # Opaque ids are passed as their own argv element and never
        # interpolated, so this is content, not syntax. Rejecting it would be
        # the bug — it mirrors what Planar guarantees for candidate ids.
        hostile = "--opaque;$(whoami)&&rm-rf/"
        self.assertEqual(harness.validate_model_value("delegated_candidate", hostile), hostile)

    def test_model_argv_emits_separate_elements(self) -> None:
        # The value must be its own argv element. An f-string here would turn
        # an opaque id into shell syntax the moment it contained a space.
        self.assertEqual(harness.model_argv("claude-opus-5"), ["--model", "claude-opus-5"])
        self.assertEqual(harness.model_argv(None), [])
        argv = harness.model_argv("--opaque;$(whoami)")
        self.assertEqual(len(argv), 2)
        self.assertEqual(argv[1], "--opaque;$(whoami)")


class TranscriptTests(unittest.TestCase):
    def test_codex_final_is_raw_multiline_text(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            raw = artifact_dir / "transcript.jsonl"
            raw.write_text(
                "\n".join(
                    [
                        "Reading additional input from stdin...",
                        json.dumps(
                            {
                                "type": "item.completed",
                                "item": {
                                    "type": "agent_message",
                                    "text": "first",
                                },
                            }
                        ),
                        json.dumps(
                            {
                                "type": "item.completed",
                                "item": {
                                    "type": "agent_message",
                                    "text": "row one\nrow two",
                                },
                            }
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            final = harness.extract_host_transcript(raw, "codex", artifact_dir)
            self.assertEqual(final, "row one\nrow two")
            self.assertEqual(
                (artifact_dir / "final.txt").read_text(encoding="utf-8"),
                "row one\nrow two\n",
            )


class ProcessTests(unittest.TestCase):
    def test_timeout_terminates_process_group(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            output = Path(raw_tmp) / "output.jsonl"
            started = time.monotonic()
            rc = harness.run_to_file(
                [sys.executable, "-c", "import time; time.sleep(10)"],
                output,
                cwd=Path(raw_tmp),
                env=os.environ.copy(),
                timeout_seconds=1,
            )
            self.assertEqual(rc, 124)
            self.assertLess(time.monotonic() - started, 7)


class ArenaIsolationBeforeHostStartTests(unittest.TestCase):
    """A poisoned arena env must fail closed before any host process starts.

    `run_phase3_preview` calls `arena.assert_isolated` immediately after
    `arena.make_arena`, before `git init`, `planar init`, or the vendor
    host invocation (`run_to_file`). Proving the exception TYPE is raised
    is not enough — a caller could catch it and continue. This proves the
    call never reaches `run_command` or `run_to_file` at all.
    """

    def test_poisoned_env_is_rejected_before_run_to_file_in_the_live_path(self) -> None:
        case = {
            "id": "isolation-guard-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant prompt text",
                "expected_models": {
                    "codex": "gpt-5.6-terra",
                    "claude": "claude-sonnet-5",
                },
            },
        }

        # Captured before patching: `arena.make_arena` is about to become a
        # Mock, so calling it BY NAME from inside the side effect below
        # would recurse into itself instead of building a real arena.
        real_make_arena = arena.make_arena

        def leaking_make_arena(root: Path, base_env=None) -> dict[str, str]:
            env = real_make_arena(root, base_env=base_env)
            env["HOME"] = str(Path.home())  # leak outside the arena root
            return env

        with tempfile.TemporaryDirectory(prefix="planar-eval-isolation-guard-") as results_root:
            options = harness.Options(
                mode="live",
                vendor="codex",
                surface="skill",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            with mock.patch.object(
                harness.arena, "make_arena", side_effect=leaking_make_arena
            ), mock.patch.object(harness, "run_to_file") as run_to_file_mock, \
                mock.patch.object(harness, "run_command") as run_command_mock:
                with self.assertRaises(arena.ArenaIsolationError) as ctx:
                    harness.run_phase3_preview(
                        Path("<isolation-guard-probe>"), case, options
                    )
                self.assertIn("HOME", str(ctx.exception))
            run_to_file_mock.assert_not_called()
            run_command_mock.assert_not_called()


class VendorStagingBeforeHostStartTests(unittest.TestCase):
    """A missing REQUIRED vendor surface must fail closed before the host starts.

    Mirrors `ArenaIsolationBeforeHostStartTests` for the staging step added
    for Planar question 983. `evals/orchestrator/test_arena.py` already
    proves `arena.stage_vendor_config` itself rejects a missing required
    surface — that is necessary but not sufficient: it does not prove
    either LIVE call site (`run_phase3_preview`, `prepare_lifecycle_fixture`)
    actually calls it before spawning a host. A no-op in place of either
    call would leave `stage_vendor_config`'s own tests green while the
    live path silently ran against an unstaged (or wrong) vendor config.
    """

    def test_run_phase3_preview_claude_fails_closed_on_missing_commands(self) -> None:
        case = {
            "id": "staging-guard-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant prompt text",
                "expected_models": {
                    "codex": "gpt-5.6-terra",
                    "claude": "claude-sonnet-5",
                },
            },
        }
        with tempfile.TemporaryDirectory(
            prefix="planar-eval-staging-guard-"
        ) as results_root, tempfile.TemporaryDirectory(
            prefix="planar-eval-staging-guard-fakehome-"
        ) as fake_home:
            # Created, but deliberately empty: no `commands` subdirectory,
            # so `claude`'s one REQUIRED surface is missing.
            fake_real_root = Path(fake_home) / "fake-claude"
            fake_real_root.mkdir()
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            with mock.patch.object(
                arena, "real_vendor_root", return_value=fake_real_root
            ), mock.patch.object(harness, "run_to_file") as run_to_file_mock, \
                mock.patch.object(harness, "run_command") as run_command_mock:
                with self.assertRaises(arena.VendorStagingError) as ctx:
                    harness.run_phase3_preview(
                        Path("<staging-guard-probe>"), case, options
                    )
                self.assertIn("commands", str(ctx.exception))
            run_to_file_mock.assert_not_called()
            run_command_mock.assert_not_called()

    def test_prepare_lifecycle_fixture_codex_fails_closed_on_missing_auth(self) -> None:
        case = {
            "id": "staging-guard-lifecycle-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "setup": {
                "fixture": "controlled-classic",
                "specialist_scenario": "baseline",
            },
        }
        with tempfile.TemporaryDirectory(
            prefix="planar-eval-staging-guard-"
        ) as results_root, tempfile.TemporaryDirectory(
            prefix="planar-eval-staging-guard-fakehome-"
        ) as fake_home:
            # Created, but deliberately empty: no `auth.json`, so codex's
            # one REQUIRED surface is missing.
            fake_real_root = Path(fake_home) / "fake-codex"
            fake_real_root.mkdir()
            options = harness.Options(
                mode="lifecycle",
                vendor="codex",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            with mock.patch.object(
                arena, "real_vendor_root", return_value=fake_real_root
            ), mock.patch.object(harness, "run_to_file") as run_to_file_mock, \
                mock.patch.object(harness, "run_command") as run_command_mock:
                with self.assertRaises(arena.VendorStagingError) as ctx:
                    harness.prepare_lifecycle_fixture(
                        Path("<staging-guard-lifecycle-probe>"),
                        case,
                        options,
                        "fixture",
                    )
                self.assertIn("auth.json", str(ctx.exception))
            run_to_file_mock.assert_not_called()
            run_command_mock.assert_not_called()

    def test_run_phase3_preview_threads_options_surface_to_staging(self) -> None:
        # Proves the CALL SITE passes options.surface through, not just
        # that arena.stage_vendor_config honors it in isolation (that is
        # test_arena.py's job). A call site that dropped the `surface=`
        # kwarg would leave stage_vendor_config to fall back to its
        # default ("skill") and this test would fail on the assertion
        # below, even though the missing-required-surface tests above
        # would still pass.
        case = {
            "id": "surface-threading-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant prompt text",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-surface-thread-") as results_root:
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )

            class _Sentinel(Exception):
                pass

            stage_mock = mock.Mock(side_effect=_Sentinel)
            with mock.patch.object(
                harness.arena, "stage_vendor_config", stage_mock
            ):
                with self.assertRaises(_Sentinel):
                    harness.run_phase3_preview(
                        Path("<surface-threading-probe>"), case, options
                    )
            self.assertEqual(stage_mock.call_count, 1)
            _, kwargs = stage_mock.call_args
            self.assertEqual(kwargs.get("surface"), "agent")

    def test_prepare_lifecycle_fixture_threads_options_surface_to_staging(self) -> None:
        case = {
            "id": "surface-threading-lifecycle-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "setup": {
                "fixture": "controlled-classic",
                "specialist_scenario": "baseline",
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-surface-thread-") as results_root:
            options = harness.Options(
                mode="lifecycle",
                vendor="codex",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )

            class _Sentinel(Exception):
                pass

            stage_mock = mock.Mock(side_effect=_Sentinel)
            with mock.patch.object(
                harness.arena, "stage_vendor_config", stage_mock
            ):
                with self.assertRaises(_Sentinel):
                    harness.prepare_lifecycle_fixture(
                        Path("<surface-threading-lifecycle-probe>"),
                        case,
                        options,
                        "fixture",
                    )
            self.assertEqual(stage_mock.call_count, 1)
            _, kwargs = stage_mock.call_args
            self.assertEqual(kwargs.get("surface"), "agent")


class VendorAuthBeforeHostStartTests(unittest.TestCase):
    """Missing vendor auth must fail closed before the host starts.

    Mirrors `VendorStagingBeforeHostStartTests` for `assert_vendor_auth()`
    (Planar artifact 626 / task 6872): staging a vendor's read surfaces is
    not the same as authenticating it, and `test_arena.py` already proves
    `arena.assert_vendor_auth()` itself rejects missing auth. That is
    necessary but not sufficient — it does not prove any live call site
    actually invokes it before spawning a host. A no-op in place of the
    `assert_vendor_auth()` call at a call site would leave
    `test_arena.py`'s own tests green while the live path silently ran
    with no auth check at all.

    Each "invokes" test below is the call-site mutant-kill evidence: it
    patches `arena.assert_vendor_auth` with a sentinel-raising mock and
    proves the call site actually reaches it (in call order, after
    `stage_vendor_config`). Deleting or no-op-ing the real call site
    invocation would make these tests fail, since the sentinel would
    never fire.
    """

    def test_run_phase3_preview_invokes_assert_vendor_auth_after_staging(self) -> None:
        case = {
            "id": "auth-guard-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant prompt text",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-auth-guard-") as results_root:
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )

            class _Sentinel(Exception):
                pass

            call_order: list[str] = []
            stage_mock = mock.Mock(
                side_effect=lambda *a, **k: call_order.append("stage")
            )

            def _auth_side_effect(*a, **k) -> None:
                call_order.append("auth")
                raise _Sentinel

            auth_mock = mock.Mock(side_effect=_auth_side_effect)
            with mock.patch.object(
                harness.arena, "stage_vendor_config", stage_mock
            ), mock.patch.object(
                harness.arena, "assert_vendor_auth", auth_mock
            ), mock.patch.object(
                harness, "run_to_file"
            ) as run_to_file_mock, mock.patch.object(
                harness, "run_command"
            ) as run_command_mock:
                with self.assertRaises(_Sentinel):
                    harness.run_phase3_preview(
                        Path("<auth-guard-probe>"), case, options
                    )
            self.assertEqual(auth_mock.call_count, 1)
            self.assertEqual(call_order, ["stage", "auth"])
            args, _ = auth_mock.call_args
            self.assertEqual(args[1], "claude")
            run_to_file_mock.assert_not_called()
            run_command_mock.assert_not_called()

    def test_prepare_lifecycle_fixture_invokes_assert_vendor_auth_after_staging(self) -> None:
        case = {
            "id": "auth-guard-lifecycle-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "setup": {
                "fixture": "controlled-classic",
                "specialist_scenario": "baseline",
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-auth-guard-") as results_root:
            options = harness.Options(
                mode="lifecycle",
                vendor="codex",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )

            class _Sentinel(Exception):
                pass

            call_order: list[str] = []
            stage_mock = mock.Mock(
                side_effect=lambda *a, **k: call_order.append("stage")
            )

            def _auth_side_effect(*a, **k) -> None:
                call_order.append("auth")
                raise _Sentinel

            auth_mock = mock.Mock(side_effect=_auth_side_effect)
            with mock.patch.object(
                harness.arena, "stage_vendor_config", stage_mock
            ), mock.patch.object(
                harness.arena, "assert_vendor_auth", auth_mock
            ), mock.patch.object(
                harness, "run_to_file"
            ) as run_to_file_mock, mock.patch.object(
                harness, "run_command"
            ) as run_command_mock:
                with self.assertRaises(_Sentinel):
                    harness.prepare_lifecycle_fixture(
                        Path("<auth-guard-lifecycle-probe>"),
                        case,
                        options,
                        "fixture",
                    )
            self.assertEqual(auth_mock.call_count, 1)
            self.assertEqual(call_order, ["stage", "auth"])
            args, _ = auth_mock.call_args
            self.assertEqual(args[1], "codex")
            run_to_file_mock.assert_not_called()
            run_command_mock.assert_not_called()

    def test_run_phase3_preview_claude_fails_closed_with_no_token_in_env(self) -> None:
        # End-to-end with the REAL assert_vendor_auth (not sentinel-mocked):
        # a fully staged, isolated arena still refuses to spawn the host
        # when no claude auth surface is present anywhere in the arena env.
        case = {
            "id": "auth-guard-real-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant prompt text",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
            },
        }
        with tempfile.TemporaryDirectory(
            prefix="planar-eval-auth-guard-"
        ) as results_root, tempfile.TemporaryDirectory(
            prefix="planar-eval-auth-guard-fakehome-"
        ) as fake_home:
            fake_real_root = Path(fake_home) / "fake-claude"
            (fake_real_root / "commands").mkdir(parents=True)
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            # A base_env carrying none of the recognized auth vars, so
            # make_arena's pass-through allowlist copies nothing that
            # would satisfy assert_vendor_auth. Captured BEFORE patching:
            # `arena.make_arena` is about to become a Mock, so calling it
            # BY NAME from inside the side effect would recurse into
            # itself instead of building a real arena.
            scrubbed_base_env = {"PATH": os.environ.get("PATH", "")}
            real_make_arena = arena.make_arena

            def scrubbed_make_arena(root, base_env=None):
                return real_make_arena(root, base_env=scrubbed_base_env)

            with mock.patch.object(
                arena, "real_vendor_root", return_value=fake_real_root
            ), mock.patch.object(
                harness.arena, "make_arena", side_effect=scrubbed_make_arena
            ), mock.patch.object(
                harness, "run_to_file"
            ) as run_to_file_mock, mock.patch.object(
                harness, "run_command"
            ) as run_command_mock:
                with self.assertRaises(arena.VendorAuthError) as ctx:
                    harness.run_phase3_preview(
                        Path("<auth-guard-real-probe>"), case, options
                    )
                message = str(ctx.exception)
                self.assertIn("CLAUDE_CODE_OAUTH_TOKEN", message)
                self.assertIn("ANTHROPIC_API_KEY", message)
            run_to_file_mock.assert_not_called()
            run_command_mock.assert_not_called()

    def test_prepare_lifecycle_fixture_replay_never_requires_auth(self) -> None:
        # options.vendor == "" is fixture replay; it must not call
        # assert_vendor_auth at all (it never spawns a real vendor host).
        case = {
            "id": "auth-guard-replay-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "setup": {
                "fixture": "controlled-classic",
                "specialist_scenario": "baseline",
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-auth-guard-replay-") as results_root:
            options = harness.Options(
                mode="lifecycle-fixture",
                vendor="",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )

            class _Sentinel(Exception):
                pass

            with mock.patch.object(
                harness.arena, "assert_vendor_auth"
            ) as auth_mock, mock.patch.object(
                harness, "run_command", side_effect=_Sentinel
            ):
                # A real (non-fake) case_path under ROOT, and run_command
                # stubbed to stop the function right after the
                # vendor-conditional staging/auth block (the first call
                # inside the `try:` is `git init`) -- enough to prove
                # assert_vendor_auth was never reached for replay, without
                # needing to fake out the rest of the fixture pipeline.
                with self.assertRaises(_Sentinel):
                    harness.prepare_lifecycle_fixture(
                        harness.CASES_DIR / "auth-guard-replay-probe.json",
                        case,
                        options,
                        "fixture",
                    )
            auth_mock.assert_not_called()


class SemanticGraderNegativeControlTests(unittest.TestCase):
    def assert_live_rejects(
        self,
        mutator: Callable[[Path], None],
        reason: str,
        *,
        options: harness.Options | None = None,
    ) -> None:
        active_options = options or live_options()
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_live_artifacts(artifact_dir, active_options)
            mutator(artifact_dir)
            with self.assertRaisesRegex(harness.EvalFailure, reason):
                harness.grade_live_artifacts(
                    live_case(), artifact_dir, active_options
                )

    def test_live_semantic_grader_accepts_valid_artifacts(self) -> None:
        for options in (live_options("codex"), live_options("claude")):
            with self.subTest(vendor=options.vendor):
                with tempfile.TemporaryDirectory() as raw_tmp:
                    artifact_dir = Path(raw_tmp)
                    write_live_artifacts(artifact_dir, options)
                    harness.grade_live_artifacts(
                        live_case(), artifact_dir, options
                    )

    def test_live_semantic_grader_negative_controls(self) -> None:
        controls = [
            (
                "missing-artifact",
                lambda root: (root / "after.git-state").unlink(),
                "artifact set is incomplete",
            ),
            (
                "dispatch-mutation",
                lambda root: harness.write_text(
                    root / "after.dispatch-state.json", "changed\n"
                ),
                "dispatch state changed before approval",
            ),
            (
                "planar-mutation",
                lambda root: harness.write_text(
                    root / "after.planar.sql", "changed\n"
                ),
                "Planar state changed before approval",
            ),
            (
                "git-mutation",
                lambda root: harness.write_text(
                    root / "after.git-state", "changed\n"
                ),
                "Git or worktree files changed before approval",
            ),
            (
                "early-claim",
                lambda root: harness.write_json(
                    root / "claims.after.json", {"active": [{"id": "claim-1"}]}
                ),
                "claim created before approval",
            ),
            (
                "missing-task-identity",
                lambda root: replace_text(
                    root / "final.txt", "task-one", "task-omitted"
                ),
                "preview omitted task slug",
            ),
            (
                "cross-vendor-task-model",
                lambda root: replace_text(
                    root / "final.txt", "gpt-5.6-terra", "claude-sonnet-5"
                ),
                "proposed a Claude model on Codex",
            ),
            (
                "missing-required-term",
                lambda root: replace_text(
                    root / "final.txt", "classic", "standard"
                ),
                "preview omitted required term",
            ),
            (
                "wrong-tier",
                lambda root: replace_text(root / "final.txt", "medium", "small"),
                "preview did not propose medium tier",
            ),
            (
                "forbidden-work-type",
                lambda root: harness.write_text(
                    root / "final.txt",
                    (root / "final.txt").read_text(encoding="utf-8")
                    + "work type: architectural\n",
                ),
                "preview assigned forbidden work type",
            ),
            (
                "missing-gate",
                lambda root: replace_text(
                    root / "final.txt", "confirm", "ready"
                ),
                "preview did not stop at an explicit gate",
            ),
            (
                "invalid-full-history-fork",
                lambda root: harness.write_text(
                    root / "transcript.txt",
                    "Full-history forked agents inherit the parent agent type\n",
                ),
                "incompatible full-history fork",
            ),
            (
                "invalid-host-command",
                lambda root: harness.write_text(
                    root / "transcript.txt", "UnknownSubcommand\n"
                ),
                "invalid command or missing reference",
            ),
            (
                "direct-agent-skill-leak",
                lambda root: harness.write_text(
                    root / "transcript.jsonl", "orchestrator/SKILL.md\n"
                ),
                "leaked the skill into the parent context",
            ),
            (
                "missing-routed-model",
                lambda root: replace_text(
                    root / "final.txt", "gpt-5.6-terra", "model-unset"
                ),
                "preview did not bind expected model",
            ),
            (
                "cross-host-model-assignment",
                lambda root: harness.write_text(
                    root / "final.txt",
                    (root / "final.txt").read_text(encoding="utf-8")
                    + "model: claude-opus-5\n",
                ),
                "preview assigned a Claude model on Codex",
            ),
        ]
        for name, mutator, reason in controls:
            with self.subTest(control=name):
                self.assert_live_rejects(mutator, reason)

    def test_claude_task_row_rejects_codex_model(self) -> None:
        options = live_options("claude")
        self.assert_live_rejects(
            lambda root: replace_text(
                root / "final.txt", "claude-sonnet-5", "gpt-5.6-terra"
            ),
            "proposed a Codex model on Claude",
            options=options,
        )

    def assert_lifecycle_rejects(
        self, mutator: Callable[[Path], None], reason: str
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            mutator(artifact_dir)
            with self.assertRaisesRegex(harness.EvalFailure, reason):
                harness.grade_lifecycle_artifacts(
                    lifecycle_case(), artifact_dir, lifecycle_options()
                )

    def test_lifecycle_semantic_grader_accepts_valid_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.grade_lifecycle_artifacts(
                lifecycle_case(), artifact_dir, lifecycle_options()
            )

    def test_lifecycle_semantic_grader_negative_controls(self) -> None:
        def change_events(root: Path, events: list[str]) -> None:
            harness.write_json(
                root / "events.normalized.json",
                [{"event": event} for event in events],
            )

        controls = [
            (
                "missing-artifact",
                lambda root: (root / "fixture-test.json").unlink(),
                "artifact set is incomplete",
            ),
            (
                "event-order",
                lambda root: change_events(
                    root,
                    [
                        "claim-acquired",
                        "coder-finished",
                        "task-completed",
                        "review-approved",
                    ],
                ),
                "ordered event missing",
            ),
            (
                "forbidden-event",
                lambda root: change_events(
                    root,
                    [
                        "claim-acquired",
                        "coder-finished",
                        "review-approved",
                        "task-completed",
                        "task-failed",
                    ],
                ),
                "forbidden event observed",
            ),
            (
                "event-count",
                lambda root: change_events(
                    root,
                    [
                        "claim-acquired",
                        "coder-finished",
                        "coder-finished",
                        "review-approved",
                        "task-completed",
                    ],
                ),
                "event coder-finished count was 2",
            ),
            (
                "task-status",
                lambda root: harness.write_json(
                    root / "task.after.json", {"status": "in_progress"}
                ),
                "task status was in_progress",
            ),
            (
                "active-claim",
                lambda root: harness.write_json(
                    root / "claims.after.json", {"active": [{"id": "claim-1"}]}
                ),
                "active claim count was 1",
            ),
            (
                "file-value",
                lambda root: harness.write_text(
                    root / "repo" / "src" / "value.txt", "rejected\n"
                ),
                "fixture value was rejected",
            ),
            (
                "fixture-test-result",
                lambda root: harness.write_json(
                    root / "fixture-test.json", {"returncode": 1}
                ),
                "fixture test exit code was 1",
            ),
        ]
        for name, mutator, reason in controls:
            with self.subTest(control=name):
                self.assert_lifecycle_rejects(mutator, reason)


class AssertionSelfTestTests(unittest.TestCase):
    """Task 6835 (C3 / D4): every contract assertion gets a per-assertion
    seeded-violation self-test, and the self-test count must equal the
    assertion count -- an assertion whose self-test cannot be constructed is
    a suite failure, never a silent skip.
    """

    def test_every_real_assertion_has_a_falsifiable_self_test(self) -> None:
        cases = harness.load_cases()
        total_declared = sum(
            len(case["contract_assertions"])
            for _, case in cases
            if "contract" in case["tiers"]
        )
        self_tests_run, total_assertions, failures = harness.run_assertion_selftests(
            cases
        )
        self.assertGreater(total_assertions, 0)
        self.assertEqual(self_tests_run, total_assertions)
        self.assertEqual(total_assertions, total_declared)
        self.assertEqual(failures, [])

    def test_main_contract_mode_invokes_run_assertion_selftests(self) -> None:
        # Drives the real CALLER (`main()` in contract mode, what `make
        # eval-orchestrator` runs) rather than calling
        # `run_assertion_selftests` directly: a no-op at this call site
        # would leave the count check unenforced by `make eval` even
        # though the function itself still works when called by hand.
        with mock.patch.object(
            harness, "run_assertion_selftests", return_value=(0, 0, [])
        ) as selftests_mock:
            code = harness.main(["--contract-only"])
        self.assertEqual(code, 0)
        selftests_mock.assert_called_once()

    def test_delete_matches_a_must_match_assertion_and_the_self_test_fails_it(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            temp_root = Path(raw_tmp)
            target = temp_root / "probe.md"
            target.write_text("alpha beta gamma\n", encoding="utf-8")
            assertion = {
                "id": "must-see-beta",
                "description": "probe",
                "mode": "all",
                "paths": ["probe.md"],
                "pattern": "beta",
            }
            harness.seed_selftest_mutation(temp_root, assertion)
            self.assertNotIn("beta", target.read_text(encoding="utf-8"))

    def test_seed_forbidden_text_for_a_must_not_match_assertion(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            temp_root = Path(raw_tmp)
            target = temp_root / "probe.md"
            target.write_text("nothing forbidden here\n", encoding="utf-8")
            assertion = {
                "id": "no-planar-task-done",
                "description": "probe",
                "mode": "none",
                "paths": ["probe.md"],
                "pattern": "planar task done",
            }
            harness.seed_selftest_mutation(temp_root, assertion)
            self.assertIn("planar task done", target.read_text(encoding="utf-8"))

    def test_unconstructable_none_pattern_is_a_suite_failure_not_a_skip(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            temp_root = Path(raw_tmp)
            (temp_root / "probe.md").write_text("x\n", encoding="utf-8")
            assertion = {
                "id": "unconstructable",
                "description": "probe",
                "mode": "none",
                "paths": ["probe.md"],
                # A character class has no single deterministic literal.
                "pattern": "[abc]xyz",
            }
            with self.assertRaisesRegex(
                harness.SelfTestConstructionError, "unconstructable"
            ):
                harness.seed_selftest_mutation(temp_root, assertion)

    def test_must_match_pattern_absent_from_every_path_is_a_suite_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            temp_root = Path(raw_tmp)
            (temp_root / "probe.md").write_text("nothing to see\n", encoding="utf-8")
            assertion = {
                "id": "never-present",
                "description": "probe",
                "mode": "all",
                "paths": ["probe.md"],
                "pattern": "ZZZ_NEVER_HERE",
            }
            with self.assertRaisesRegex(
                harness.SelfTestConstructionError, "never-present"
            ):
                harness.seed_selftest_mutation(temp_root, assertion)

    def test_run_one_assertion_selftest_fails_when_seeding_does_not_falsify(
        self,
    ) -> None:
        # A "none" assertion whose forbidden text is seeded into a path
        # OTHER than the one the assertion actually checks proves grading
        # still passes -- run_one_assertion_selftest must surface that as
        # "self-test did not fail", not silently accept it.
        with mock.patch.object(harness, "seed_selftest_mutation", lambda *_: None):
            assertion = {
                "id": "no-op-seed",
                "description": "probe",
                "mode": "none",
                "paths": ["agents/does-not-exist-for-this-probe.md"],
                "pattern": "ZZZ",
            }
            # Missing path makes grade_contract raise for a different
            # reason (missing path), which the self-test treats as a pass
            # of "grader rejected" -- so use a real, harmless path instead.
            assertion["paths"] = ["agents/methodology.md"]
            with self.assertRaisesRegex(
                harness.EvalFailure, "self-test did not fail"
            ):
                harness.run_one_assertion_selftest("probe-case", assertion)


class AggregateContractReportTests(unittest.TestCase):
    """Task 6840 / C6: contract mode collects every failing case into one
    aggregate report instead of stopping at the first `EvalFailure`. These
    drive the real CALLER (`main(["--contract-only"])`, what `make
    eval-orchestrator` runs), not `collect_case_failures` in isolation, so
    a caller that reverts to raising on the first failure is caught here.
    """

    def test_two_seeded_failures_and_a_pass_all_run_and_summarize(self) -> None:
        cases = harness.load_cases()
        contract_entries = [
            (path, case) for path, case in cases if "contract" in case["tiers"]
        ]
        self.assertGreaterEqual(len(contract_entries), 3)
        fail_ids = {contract_entries[0][1]["id"], contract_entries[1][1]["id"]}
        pass_id = contract_entries[2][1]["id"]
        seen: list[str] = []
        real_grade_contract = harness.grade_contract

        def fake_grade_contract(path: Path, case: dict[str, object]) -> None:
            # Only intercept OUR seeded cases; everything else (including
            # `grade_contract_negative_control`'s own synthetic probes,
            # which call this same module-global name) delegates to the
            # real grader so this test cannot pass by accident.
            if case["id"] in {contract_entries[i][1]["id"] for i in range(3)}:
                seen.append(case["id"])
            if case["id"] in fail_ids:
                raise harness.EvalFailure(f"seeded failure: {case['id']}")
            real_grade_contract(path, case)

        with mock.patch.object(
            harness, "grade_contract", side_effect=fake_grade_contract
        ), mock.patch.object(
            harness, "run_assertion_selftests", return_value=(0, 0, [])
        ):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = harness.main(["--contract-only"])

        # All three watched cases ran -- the two seeded failures did not
        # stop the loop before the pass case was graded.
        self.assertEqual(
            set(seen), {contract_entries[i][1]["id"] for i in range(3)}
        )
        self.assertIn(pass_id, seen)
        self.assertEqual(code, 1)
        output = stderr.getvalue()
        self.assertIn(f"FAIL SUMMARY: 2 of {len(contract_entries)}", output)
        for fail_id in fail_ids:
            self.assertIn(f"fail: {fail_id}", output)
        self.assertNotIn(f"fail: {pass_id}", output)

    def test_blocked_only_batch_exits_75_and_is_listed_as_blocked(self) -> None:
        cases = harness.load_cases()
        contract_entries = [
            (path, case) for path, case in cases if "contract" in case["tiers"]
        ]
        blocked_id = contract_entries[0][1]["id"]
        real_grade_contract = harness.grade_contract

        def fake_grade_contract(path: Path, case: dict[str, object]) -> None:
            if case["id"] == blocked_id:
                raise harness.EvalBlocked("seeded block")
            real_grade_contract(path, case)

        with mock.patch.object(
            harness, "grade_contract", side_effect=fake_grade_contract
        ), mock.patch.object(
            harness, "run_assertion_selftests", return_value=(0, 0, [])
        ):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = harness.main(["--contract-only"])

        self.assertEqual(code, 75)
        self.assertIn(f"blocked: {blocked_id}", stderr.getvalue())

    def test_a_hard_failure_outranks_a_block_in_the_same_batch(self) -> None:
        cases = harness.load_cases()
        contract_entries = [
            (path, case) for path, case in cases if "contract" in case["tiers"]
        ]
        self.assertGreaterEqual(len(contract_entries), 2)
        fail_id = contract_entries[0][1]["id"]
        blocked_id = contract_entries[1][1]["id"]
        real_grade_contract = harness.grade_contract

        def fake_grade_contract(path: Path, case: dict[str, object]) -> None:
            if case["id"] == fail_id:
                raise harness.EvalFailure("seeded failure")
            if case["id"] == blocked_id:
                raise harness.EvalBlocked("seeded block")
            real_grade_contract(path, case)

        with mock.patch.object(
            harness, "grade_contract", side_effect=fake_grade_contract
        ), mock.patch.object(
            harness, "run_assertion_selftests", return_value=(0, 0, [])
        ):
            code = harness.main(["--contract-only"])

        self.assertEqual(code, 1)

    def test_single_case_filter_summary_still_names_the_failure(self) -> None:
        # `--case` selection keeps the same per-case failure content; the
        # only change from pre-aggregation behaviour is the summary line
        # wrapping it (task 6840's single-case parity requirement).
        cases = harness.load_cases()
        contract_entries = [
            (path, case) for path, case in cases if "contract" in case["tiers"]
        ]
        target_id = contract_entries[0][1]["id"]
        real_grade_contract = harness.grade_contract

        def fake_grade_contract(path: Path, case: dict[str, object]) -> None:
            if case["id"] == target_id:
                raise harness.EvalFailure(f"seeded failure: {case['id']}")
            real_grade_contract(path, case)

        with mock.patch.object(
            harness, "grade_contract", side_effect=fake_grade_contract
        ), mock.patch.object(
            harness, "run_assertion_selftests", return_value=(0, 0, [])
        ):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = harness.main(["--contract-only", "--case", target_id])

        self.assertEqual(code, 1)
        self.assertIn(f"FAIL SUMMARY: 1 of 1", stderr.getvalue())
        self.assertIn(f"seeded failure: {target_id}", stderr.getvalue())


class RegradeWritesDatedSiblingTests(unittest.TestCase):
    """Task 6840 / C6: regrading retained artifacts must never overwrite
    the original run's `grade.json` -- it writes a new dated file next to
    it instead.
    """

    def test_regrade_writes_a_dated_sibling_and_leaves_grade_json_untouched(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp).resolve()
            write_lifecycle_artifacts(artifact_dir)
            original_grade = {
                "status": "fail",
                "case_id": lifecycle_case()["id"],
                "vendor": "",
                "surface": "agent",
                "mode": "lifecycle-fixture",
                "reason": "original run failed",
            }
            harness.write_json(artifact_dir / "grade.json", original_grade)
            harness.write_json(
                artifact_dir / "run.json",
                {
                    "mode": "lifecycle-fixture",
                    "vendor": "",
                    "surface": "agent",
                    "case_id": lifecycle_case()["id"],
                },
            )
            original_bytes = (artifact_dir / "grade.json").read_bytes()

            regrade_path = harness.regrade_artifacts(
                artifact_dir, [(Path("case.json"), lifecycle_case())]
            )

            self.assertNotEqual(regrade_path, artifact_dir / "grade.json")
            self.assertTrue(regrade_path.is_file())
            self.assertEqual(regrade_path.parent, artifact_dir)
            self.assertRegex(regrade_path.name, r"^grade\.\d{8}T\d{6}\d*Z\.json$")
            # The original verdict is byte-for-byte untouched.
            self.assertEqual(
                (artifact_dir / "grade.json").read_bytes(), original_bytes
            )
            regraded = json.loads(regrade_path.read_text(encoding="utf-8"))
            self.assertEqual(regraded["status"], "pass")
            self.assertEqual(regraded["case_id"], lifecycle_case()["id"])

    def test_regrading_twice_produces_two_distinct_dated_files(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.write_json(
                artifact_dir / "run.json",
                {
                    "mode": "lifecycle-fixture",
                    "vendor": "",
                    "surface": "agent",
                    "case_id": lifecycle_case()["id"],
                },
            )
            first = harness.regrade_artifacts(
                artifact_dir, [(Path("case.json"), lifecycle_case())]
            )
            second = harness.regrade_artifacts(
                artifact_dir, [(Path("case.json"), lifecycle_case())]
            )
            self.assertNotEqual(first, second)
            self.assertTrue(first.is_file())
            self.assertTrue(second.is_file())


FIXTURE_ROOT = (
    Path(__file__).resolve().parent / "fixtures" / "controlled-classic"
)


def observed_record(
    *,
    ts: float,
    seq: int,
    argv: list[str],
    exit_code: int = 0,
    stdout: object = "",
    stderr: str = "",
    pid: int | None = None,
) -> dict[str, object]:
    return {
        "ts": ts,
        "seq": seq,
        "pid": pid if pid is not None else 1000 + seq,
        "argv": argv,
        "exit_code": exit_code,
        "stdout": stdout,
        "stderr": stderr,
    }


def write_observed_record(observed_dir: Path, record: dict[str, object]) -> None:
    observed_dir.mkdir(parents=True, exist_ok=True)
    name = f"{record['ts']}-{record['pid']}-{record['seq']}.json"
    (observed_dir / name).write_text(json.dumps(record) + "\n", encoding="utf-8")


# The documented `planar-agent --json` shapes (docs/cli-reference.md § JSON
# shapes): `pull` -> {ok, no_work, claim_token?, claim?, task?, action_id?};
# `complete|fail|release|block` -> {ok, claim_token, claim, task};
# `claim|heartbeat` -> {ok, claim_token, claim}. The task id lives at
# `task.id` (fallback `claim.entity_id`), never a flat `task_id` -- these
# helpers build fixtures in that real nested shape rather than one the
# product never produces, per F1.
def pull_stdout(
    *,
    claim_token: str | None = None,
    task_id: str | None = None,
    no_work: bool = False,
) -> dict[str, object]:
    if no_work:
        return {"ok": True, "no_work": True}
    return {
        "ok": True,
        "no_work": False,
        "claim_token": claim_token,
        "claim": {"entity_id": task_id},
        "task": {"id": task_id},
    }


def claim_stdout(*, claim_token: str, task_id: str | None = None) -> dict[str, object]:
    return {"ok": True, "claim_token": claim_token, "claim": {"entity_id": task_id}}


def terminal_stdout(
    *, claim_token: str, task_id: str | None = None
) -> dict[str, object]:
    return {
        "ok": True,
        "claim_token": claim_token,
        "claim": {"entity_id": task_id},
        "task": {"id": task_id},
    }


class WrapperObservedLogTest(unittest.TestCase):
    """C2 (task hh-observed-log): the `controlled-classic` planar-agent PATH
    wrapper records every invocation -- including heartbeats and a failed
    terminal verb -- as its own file under observed/, JSON-encoded rather
    than string-interpolated.
    """

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.eval_dir = Path(self.tmp.name) / ".eval"
        (self.eval_dir / "bin").mkdir(parents=True)
        shutil.copy2(
            FIXTURE_ROOT / "planar-agent", self.eval_dir / "bin" / "planar-agent"
        )
        shutil.copy2(
            FIXTURE_ROOT / "record_observed.py",
            self.eval_dir / "record_observed.py",
        )
        os.chmod(self.eval_dir / "bin" / "planar-agent", 0o755)
        os.chmod(self.eval_dir / "record_observed.py", 0o755)
        # A stub `real-planar-agent`: `complete` succeeds once (exit 0, JSON
        # stdout in the documented nested shape -- docs/cli-reference.md §
        # JSON shapes: `{ok, claim_token, claim, task}`) and fails on any
        # later invocation (non-zero, stderr prose) so the wrapper's
        # fail-forwarding path is exercised without touching a real
        # planar-agent binary. Any other command (e.g. `heartbeat`) returns
        # the `claim|heartbeat` shape `{ok, claim_token, claim}`.
        stub = self.eval_dir / "fake-real-planar-agent"
        stub.write_text(
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            'if [ "$1" = "complete" ] && [ ! -f "'
            + str(self.eval_dir / ".completed")
            + '" ]; then\n'
            '  touch "' + str(self.eval_dir / ".completed") + '"\n'
            "  echo '{\"ok\":true,\"claim_token\":\"tok-1\","
            "\"claim\":{\"entity_id\":\"6831\"},\"task\":{\"id\":\"6831\"}}'\n"
            "  exit 0\n"
            'elif [ "$1" = "complete" ]; then\n'
            '  echo "already completed" >&2\n'
            "  exit 3\n"
            "else\n"
            "  echo '{\"ok\":true,\"claim_token\":\"tok-1\","
            "\"claim\":{\"entity_id\":\"6831\"}}'\n"
            "  exit 0\n"
            "fi\n",
            encoding="utf-8",
        )
        os.chmod(stub, 0o755)
        (self.eval_dir / "real-planar-agent").write_text(
            str(stub) + "\n", encoding="utf-8"
        )
        self.env = dict(os.environ)
        self.env["PATH"] = str(self.eval_dir / "bin") + os.pathsep + self.env["PATH"]

    def run_wrapper(self, args: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.eval_dir / "bin" / "planar-agent"), *args],
            cwd=self.eval_dir,
            env=self.env,
            capture_output=True,
            text=True,
            check=False,
        )

    def observed_records(self) -> list[dict[str, object]]:
        return harness.read_observed_records(self.eval_dir / "observed")

    def test_heartbeat_and_failed_terminal_verb_both_appear(self) -> None:
        self.run_wrapper(["heartbeat", "--claim", "tok-1", "--ttl", "8h", "--json"])
        self.run_wrapper(["complete", "--claim", "tok-1", "--json"])
        second = self.run_wrapper(["complete", "--claim", "tok-1", "--json"])
        self.assertEqual(second.returncode, 3)
        records = self.observed_records()
        self.assertEqual(len(records), 3)
        heartbeat, first_complete, second_complete = records
        self.assertEqual(heartbeat["argv"][0], "heartbeat")
        self.assertEqual(heartbeat["exit_code"], 0)
        self.assertIn("ts", heartbeat)
        self.assertIn("argv", heartbeat)
        self.assertIn("exit_code", heartbeat)
        self.assertEqual(first_complete["exit_code"], 0)
        self.assertEqual(first_complete["stdout"]["claim_token"], "tok-1")
        self.assertEqual(second_complete["exit_code"], 3)
        self.assertIn("already completed", second_complete["stderr"])

    def test_sixteen_concurrent_calls_leave_sixteen_intact_records(self) -> None:
        # Task 6877: the wrapper no longer serializes concurrent lanes
        # through a cross-process lock at all -- `record_observed.py`'s
        # `<ts>-<pid>-<seq>.json` naming plus `os.replace` (atomic rename)
        # already guarantees sixteen concurrent lanes cannot interleave or
        # clobber each other's file without one. `seq` is always 0 now, so
        # the deterministic assertion is DISTINCT FILENAMES (one per pid,
        # since pid is what actually varies across the sixteen lanes) and
        # sixteen intact records, not sixteen distinct seq values.
        big_summary = "x" * 70_000
        procs = [
            subprocess.Popen(
                [
                    str(self.eval_dir / "bin" / "planar-agent"),
                    "heartbeat",
                    "--claim",
                    f"tok-{i}",
                    "--ttl",
                    "8h",
                    "--summary",
                    big_summary,
                    "--json",
                ],
                cwd=self.eval_dir,
                env=self.env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            for i in range(16)
        ]
        for proc in procs:
            self.assertEqual(proc.wait(timeout=30), 0)
        observed_dir = self.eval_dir / "observed"
        filenames = sorted(
            p.name for p in observed_dir.glob("*.json") if not p.name.startswith(".tmp-")
        )
        self.assertEqual(len(filenames), 16, f"expected 16 distinct files: {filenames}")
        self.assertEqual(len(set(filenames)), 16, "filenames were not all distinct")
        records = self.observed_records()
        self.assertEqual(len(records), 16)
        self.assertTrue(all(record["seq"] == 0 for record in records))
        pids = {record["pid"] for record in records}
        self.assertEqual(len(pids), 16, "expected 16 distinct wrapper pids")
        for record in records:
            self.assertEqual(record["argv"].count(big_summary), 1)

    def test_record_failure_writes_sentinel_and_still_forwards_exit_code(
        self,
    ) -> None:
        # C2: recording is fail-OPEN (the wrapper's contract is to forward
        # the real exit code even if recording itself breaks), but a
        # dropped record must not be silently invisible to the grader.
        broken_record_script = self.eval_dir / "record_observed.py"
        broken_record_script.write_text(
            "#!/usr/bin/env python3\nimport sys\nsys.exit(9)\n", encoding="utf-8"
        )
        result = self.run_wrapper(
            ["heartbeat", "--claim", "tok-1", "--ttl", "8h", "--json"]
        )
        self.assertEqual(result.returncode, 0, "wrapper must still forward $RC")
        sentinel = self.eval_dir / "observed" / ".record-failed"
        self.assertTrue(sentinel.is_file())
        self.assertIn("record_rc=9", sentinel.read_text(encoding="utf-8"))

    def test_quotes_newlines_and_braces_round_trip(self) -> None:
        hostile = 'has "quotes", a\nnewline, and a } brace {'
        self.run_wrapper(["complete", "--claim", "tok-1", "--summary", hostile, "--json"])
        records = self.observed_records()
        self.assertEqual(len(records), 1)
        argv = records[0]["argv"]
        # Must round-trip as its OWN argv element -- a list of the exact
        # original tokens -- not merely appear as a substring somewhere in
        # the record (a naive `" ".join(argv)` interpolation would still
        # satisfy a plain containment check while destroying the token
        # boundary the grader relies on to find --claim's value, etc.).
        self.assertIsInstance(argv, list)
        self.assertEqual(
            argv, ["complete", "--claim", "tok-1", "--summary", hostile, "--json"]
        )

    def test_real_stub_nested_shape_feeds_task_id_extraction(self) -> None:
        # F1 regression guard: the stub `real-planar-agent` in setUp emits
        # the DOCUMENTED nested shape (`task.id` / `claim.entity_id`), not
        # a flat `task_id`. Feed the REAL wrapper's REAL observed records
        # (produced by the real record_observed.py, not a hand-built
        # dict) into `observed_task_id` / `assert_terminal_verb_lifecycle`
        # and confirm the task id is actually recovered -- a fixture that
        # emitted a shape the product never produces (flat `task_id`)
        # would pass this end-to-end path trivially while masking the
        # exact defect F1 fixed (`terminal_ts_by_task` never populated on
        # a real run).
        self.run_wrapper(["complete", "--claim", "tok-1", "--json"])
        records = self.observed_records()
        self.assertEqual(len(records), 1)
        self.assertEqual(harness.observed_task_id(records[0]), "6831")
        # And the claim-after-terminal gate actually fires against this
        # REAL record when a later claim on the same task is observed.
        followup = observed_record(
            ts=records[0]["ts"] + 1.0,
            seq=records[0]["seq"] + 1,
            argv=["claim", "--entity", "task:6831"],
            stdout=claim_stdout(claim_token="tok-2", task_id="6831"),
        )
        with self.assertRaisesRegex(harness.EvalFailure, "6831"):
            harness.assert_terminal_verb_lifecycle([records[0], followup])


class ForbiddenEventEmitterTests(unittest.TestCase):
    """Task 6836 (C3 / D4): a `forbidden_events` entry that no fixture path
    can ever emit makes the negative assertion unfalsifiable. `EMITTABLE_EVENTS`
    is the manifest `validate_case` checks against; this class proves the
    manifest matches the actual `control.sh` script (so it cannot silently
    drift) and that the one event control.sh emits only under a seed knob
    (`task-completed-before-review`) really is rejected by the grader when it
    fires.
    """

    def test_fixture_log_events_matches_control_script(self) -> None:
        # `FIXTURE_LOG_EVENTS` must contain exactly the string literals
        # `control.sh` passes to `log_event`, no more and no less -- a
        # manifest entry with no matching `log_event` call would itself be
        # an unemittable event smuggled past `validate_case`.
        text = (FIXTURE_ROOT / "control.sh").read_text(encoding="utf-8")
        scripted = set(re.findall(r'log_event "([a-z0-9-]+)"', text))
        self.assertEqual(scripted, harness.FIXTURE_LOG_EVENTS)

    def test_validate_case_rejects_an_unemittable_forbidden_event(self) -> None:
        case = dict(lifecycle_case())
        case.update(
            {
                "schema_version": 1,
                "id": "unemittable-forbidden-event",
                "skill": "orchestrator",
                "description": "probe",
                "tags": ["lifecycle"],
                "tiers": ["contract", "lifecycle"],
                "tasks": [{"slug": "controlled-value", "title": "t"}],
                "contract_assertions": [
                    {
                        "id": "probe",
                        "description": "probe",
                        "mode": "all",
                        "paths": ["agents/methodology.md"],
                        "pattern": r"\bthe\b",
                    }
                ],
                "setup": {
                    "fixture": "controlled-classic",
                    "specialist_scenario": "success",
                },
                "lifecycle": {
                    "scenario": "controlled-classic",
                    "surface": "agent",
                    "prompt": "probe",
                },
            }
        )
        case["expected"] = dict(case["expected"])
        case["expected"]["forbidden_events"] = ["this-event-does-not-exist"]
        with self.assertRaisesRegex(
            harness.EvalFailure, "this-event-does-not-exist"
        ):
            harness.validate_case(Path("<probe>"), case)

    def test_load_cases_calls_validate_case_and_rejects_a_bad_case_on_disk(
        self,
    ) -> None:
        # Drives the real CALLER (`load_cases`, which `main()` calls before
        # anything else runs) rather than `validate_case` directly: proves
        # the call site actually reaches the forbidden_events check, so a
        # no-op in place of either `load_cases`'s call to `validate_case` or
        # the check inside it would fail this test.
        case = dict(lifecycle_case())
        case.update(
            {
                "schema_version": 1,
                "id": "on-disk-unemittable-forbidden-event",
                "skill": "orchestrator",
                "description": "probe",
                "tags": ["lifecycle"],
                "tiers": ["contract", "lifecycle"],
                "tasks": [{"slug": "controlled-value", "title": "t"}],
                "contract_assertions": [
                    {
                        "id": "probe",
                        "description": "probe",
                        "mode": "all",
                        "paths": ["agents/methodology.md"],
                        "pattern": r"\bthe\b",
                    }
                ],
                "setup": {
                    "fixture": "controlled-classic",
                    "specialist_scenario": "success",
                },
                "lifecycle": {
                    "scenario": "controlled-classic",
                    "surface": "agent",
                    "prompt": "probe",
                },
            }
        )
        case["expected"] = dict(case["expected"])
        case["expected"]["forbidden_events"] = ["this-event-does-not-exist"]
        with tempfile.TemporaryDirectory() as raw_tmp:
            cases_dir = Path(raw_tmp)
            (cases_dir / "on-disk-unemittable-forbidden-event.json").write_text(
                json.dumps(case), encoding="utf-8"
            )
            with mock.patch.object(harness, "CASES_DIR", cases_dir):
                with self.assertRaisesRegex(
                    harness.EvalFailure, "this-event-does-not-exist"
                ):
                    harness.load_cases()

    def test_existing_cases_declare_only_emittable_forbidden_events(self) -> None:
        # Guards the actual repository cases: `classic-lifecycle-success`
        # and `classic-reviewer-bounce` must load cleanly under the new
        # validation (i.e. `task-completed-before-review` really is
        # emittable now that control.sh's seed knob exists).
        for path, case in harness.load_cases():
            if "lifecycle" not in case["tiers"]:
                continue
            forbidden = case.get("expected", {}).get("forbidden_events", [])
            unemittable = [
                name for name in forbidden if name not in harness.EMITTABLE_EVENTS
            ]
            self.assertEqual(unemittable, [], f"{path}: {unemittable}")

    def test_control_script_emits_the_seeded_violation_only_when_asked(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            repo = Path(raw_tmp) / "repo"
            shutil.copytree(FIXTURE_ROOT / "repo", repo)
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            (repo / ".eval").mkdir(exist_ok=True)
            (repo / ".eval" / "scenario").write_text("success", encoding="utf-8")
            (repo / ".eval" / "events.jsonl").write_text("", encoding="utf-8")
            env = dict(os.environ)
            env["EVAL_SEED_VIOLATION"] = "task-completed-before-review"
            result = subprocess.run(
                [str(FIXTURE_ROOT / "control.sh"), "coder", "tok-1"],
                cwd=repo,
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            events = [
                json.loads(line)
                for line in (repo / ".eval" / "events.jsonl").read_text(
                    encoding="utf-8"
                ).splitlines()
                if line.strip()
            ]
            names = [event["event"] for event in events]
            self.assertIn("task-completed-before-review", names)
            self.assertIn("coder-finished", names)
            self.assertLess(
                names.index("task-completed-before-review"),
                names.index("coder-finished"),
            )

            # Without the knob the event never fires (the emitter is a
            # deliberate self-test path, not part of a real controlled run).
            (repo / ".eval" / "events.jsonl").write_text("", encoding="utf-8")
            del env["EVAL_SEED_VIOLATION"]
            subprocess.run(
                [str(FIXTURE_ROOT / "control.sh"), "coder", "tok-1"],
                cwd=repo,
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            events = [
                json.loads(line)
                for line in (repo / ".eval" / "events.jsonl").read_text(
                    encoding="utf-8"
                ).splitlines()
                if line.strip()
            ]
            self.assertNotIn(
                "task-completed-before-review", [event["event"] for event in events]
            )

    def test_grader_rejects_a_run_where_the_seeded_event_fired(self) -> None:
        # Feed the emitted event straight into the real grading path
        # (merge_lifecycle_events -> grade_lifecycle_artifacts), exactly as
        # the fixture-replay collector would, and require the lifecycle
        # case's own forbidden_events assertion to reject it.
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            observed_dir = root / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=5.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
                ),
            )
            events_path = root / "events.jsonl"
            events_path.write_text(
                "\n".join(
                    json.dumps(event)
                    for event in [
                        {
                            "event": "task-completed-before-review",
                            "source": "controlled-coder",
                            "ts": 1.5,
                        },
                        {
                            "event": "coder-finished",
                            "source": "controlled-coder",
                            "ts": 1.6,
                        },
                        {
                            "event": "review-approved",
                            "source": "controlled-reviewer",
                            "ts": 3.0,
                        },
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            events = harness.merge_lifecycle_events(observed_dir, events_path)
            artifact_dir = root / "artifacts"
            artifact_dir.mkdir()
            harness.write_json(artifact_dir / "events.normalized.json", events)
            harness.write_json(artifact_dir / "task.after.json", {"status": "done"})
            harness.write_json(artifact_dir / "claims.after.json", {"active": []})
            harness.write_text(artifact_dir / "fixture-test.txt", "PASS\n")
            harness.write_json(artifact_dir / "fixture-test.json", {"returncode": 0})
            harness.write_text(
                artifact_dir / "repo" / "src" / "value.txt", "approved\n"
            )
            case = dict(lifecycle_case())
            case["expected"] = dict(case["expected"])
            case["expected"]["forbidden_events"] = list(
                case["expected"]["forbidden_events"]
            ) + ["task-completed-before-review"]
            with self.assertRaisesRegex(
                harness.EvalFailure, "forbidden event observed"
            ):
                harness.grade_lifecycle_artifacts(
                    case, artifact_dir, lifecycle_options()
                )


class ObservedLogOrderingTest(unittest.TestCase):
    """Task 6877: `seq` no longer varies across records (the wrapper's
    cross-process lock/counter was deleted outright, not hardened
    further -- see the wrapper's own header comment). Ordering ties are
    now broken by `pid`, which is always distinct across concurrently
    running wrapper processes, so `read_observed_records` and every place
    that derives lifecycle order from the observed log must sort by
    (ts, pid, seq), not (ts, seq) alone.
    """

    def test_read_observed_records_breaks_a_ts_tie_by_pid(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            observed_dir = Path(raw_tmp) / "observed"
            # Both records share the EXACT same ts (a real possibility now
            # that seq no longer disambiguates arrival order at all); only
            # pid differs. Write the higher-pid record FIRST on disk to
            # prove the ordering comes from the sort, not file-glob order.
            write_observed_record(
                observed_dir,
                observed_record(ts=5.0, seq=0, pid=9999, argv=["complete", "--claim", "tok-hi"]),
            )
            write_observed_record(
                observed_dir,
                observed_record(ts=5.0, seq=0, pid=1111, argv=["pull"]),
            )
            records = harness.read_observed_records(observed_dir)
            self.assertEqual([r["pid"] for r in records], [1111, 9999])

    def test_observed_sort_key_orders_by_ts_then_pid_then_seq(self) -> None:
        low_pid = observed_record(ts=5.0, seq=9, pid=100, argv=["pull"])
        high_pid = observed_record(ts=5.0, seq=0, pid=200, argv=["pull"])
        earlier_ts = observed_record(ts=4.0, seq=0, pid=999, argv=["pull"])
        ordered = sorted(
            [high_pid, low_pid, earlier_ts], key=harness.observed_sort_key
        )
        self.assertEqual([r["pid"] for r in ordered], [999, 100, 200])

    def test_merge_lifecycle_events_breaks_a_ts_tie_by_pid(self) -> None:
        # `read_observed_records` already sorts its own stream by (ts, pid,
        # seq) via the on-disk `<ts>-<pid>-<seq>.json` filename, independent
        # of what `merge_lifecycle_events` does with its own sort key -- so
        # two observed records alone cannot distinguish the merge's sort
        # key from a ts-only one; the pre-sorted input already comes out
        # right either way. Use the BOUNDARY-event stream instead (raw
        # `event_objects()` in FILE order, unsorted), with two events
        # sharing the same `ts` and only `pid` differing, written to disk
        # in the WRONG (descending-pid) order. Only a real (ts, pid, seq)
        # sort in `merge_lifecycle_events` puts the lower pid first; a
        # mutant reverting the sort key to ts-only is a stable sort that
        # preserves file order (pid 500 first) and this assertion catches
        # it (task 6883).
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            observed_dir = root / "observed"
            events_path = root / "events.jsonl"
            events_path.write_text(
                json.dumps({"event": "review-approved", "source": "controlled-reviewer", "ts": 3.0, "pid": 500})
                + "\n"
                + json.dumps({"event": "coder-finished", "source": "controlled-coder", "ts": 3.0, "pid": 100})
                + "\n",
                encoding="utf-8",
            )
            events = harness.merge_lifecycle_events(observed_dir, events_path)
            self.assertEqual([e["event"] for e in events], ["coder-finished", "review-approved"])
            self.assertEqual([e["pid"] for e in events], [100, 500])


class ObservedLogGraderTest(unittest.TestCase):
    """C2/D3 (tasks hh-drop-order-fabrication, hh-terminal-verb-assertions):
    the grader derives event order from the observed log rather than the
    audit table, and enforces the terminal-verb lifecycle invariants.
    """

    def test_empty_log_with_audit_activity_fails_as_wrapper_bypassed(self) -> None:
        with self.assertRaisesRegex(harness.EvalFailure, "wrapper-bypassed"):
            harness.assert_wrapper_not_bypassed(
                observed_records=[],
                claims=[{"status": "completed"}],
            )

    def test_empty_log_with_no_audit_activity_does_not_fail(self) -> None:
        harness.assert_wrapper_not_bypassed(observed_records=[], claims=[])

    def test_nonempty_log_with_audit_activity_does_not_fail(self) -> None:
        harness.assert_wrapper_not_bypassed(
            observed_records=[observed_record(ts=1.0, seq=0, argv=["pull"])],
            claims=[{"status": "completed"}],
        )

    def test_partial_bypass_count_mismatch_fails(self) -> None:
        # C3: the observed log is non-empty (a direct bypass never
        # happened for AT LEAST one claim), but only one of the audit's
        # two claims went through the wrapper -- a stray direct
        # `planar-agent claim` call for the second, or a lane that dropped
        # its wrapper PATH override.
        with self.assertRaisesRegex(harness.EvalFailure, "wrapper-bypassed"):
            harness.assert_wrapper_not_bypassed(
                observed_records=[
                    observed_record(
                        ts=1.0,
                        seq=0,
                        argv=["pull"],
                        stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
                    )
                ],
                claims=[{"status": "completed"}, {"status": "completed"}],
            )

    def test_plan_level_claim_does_not_count_against_task_scoped_audit(self) -> None:
        # C7: `claims` (from `planar audit trail --kind task <id>`) is
        # already task-scoped, but the observed log is the WHOLE run's
        # log -- it also carries the orchestrator's own
        # `claim --entity plan:<id>` (a real call every orchestrator run
        # makes) alongside the task-scoped claim. Without the `task_id`
        # filter, this would report an over-count (2 observed vs. 1
        # audited) as a spurious `wrapper-bypassed: partial` on a
        # perfectly clean run.
        plan_level_claim = observed_record(
            ts=1.0,
            seq=0,
            argv=["claim", "--entity", "plan:1"],
            stdout=claim_stdout(claim_token="tok-plan", task_id="1"),
        )
        task_level_claim = observed_record(
            ts=2.0,
            seq=1,
            argv=["claim", "--entity", "task:6832"],
            stdout=claim_stdout(claim_token="tok-task", task_id="6832"),
        )
        harness.assert_wrapper_not_bypassed(
            observed_records=[plan_level_claim, task_level_claim],
            claims=[{"status": "completed"}],
            task_id="6832",
        )

    def test_matching_claim_counts_do_not_fail(self) -> None:
        harness.assert_wrapper_not_bypassed(
            observed_records=[
                observed_record(
                    ts=1.0,
                    seq=0,
                    argv=["pull"],
                    stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
                ),
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["pull"],
                    stdout=pull_stdout(claim_token="tok-2", task_id="6833"),
                ),
            ],
            claims=[{"status": "completed"}, {"status": "completed"}],
        )

    def test_no_work_pull_is_not_claim_acquired(self) -> None:
        # F2: `pull` returns exit 0 with {ok:true, no_work:true} when
        # nothing is eligible (src/cmd/planar-agent/handlers/claims.cpp:
        # 194-196). A no_work pull must not be named `claim-acquired` --
        # that would hand a polling orchestrator a spurious event and, if
        # it lands after `task-completed`, would be exactly the
        # synthesized-order problem D3 deletes the audit fallback over.
        record = observed_record(
            ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(no_work=True)
        )
        self.assertIsNone(harness.observed_event_name(record))

    def test_no_work_pull_does_not_count_toward_wrapper_bypass_check(self) -> None:
        # A no_work pull observed alongside a real audit claim must not
        # mask a genuine bypass: it does not count toward the C3 observed
        # claim-acquired tally.
        with self.assertRaisesRegex(harness.EvalFailure, "wrapper-bypassed"):
            harness.assert_wrapper_not_bypassed(
                observed_records=[
                    observed_record(
                        ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(no_work=True)
                    )
                ],
                claims=[{"status": "completed"}],
            )

    def test_pull_with_claim_token_is_claim_acquired_even_if_no_work_missing(
        self,
    ) -> None:
        # A `pull` stdout carrying a claim_token is a real claim regardless
        # of how `no_work` is spelled -- `has_token` is the belt to
        # `no_work`'s suspenders per F2's "no_work falsy OR claim_token
        # present" condition.
        record = observed_record(
            ts=1.0,
            seq=0,
            argv=["pull"],
            stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
        )
        self.assertEqual(harness.observed_event_name(record), "claim-acquired")

    def test_task_id_from_nested_task_field(self) -> None:
        # F1: the real `complete --json` shape nests the task id at
        # `task.id` (docs/cli-reference.md § JSON shapes), never a flat
        # `task_id`.
        record = observed_record(
            ts=1.0,
            seq=0,
            argv=["complete", "--claim", "tok-1"],
            stdout={"ok": True, "claim_token": "tok-1", "claim": {}, "task": {"id": 6832}},
        )
        self.assertEqual(harness.observed_task_id(record), "6832")

    def test_task_id_falls_back_to_claim_entity_id(self) -> None:
        # `claim`/`heartbeat` -> {ok, claim_token, claim} has no `task`
        # key at all; the fallback is `claim.entity_id`.
        record = observed_record(
            ts=1.0,
            seq=0,
            argv=["claim", "--entity", "task:9999"],
            stdout={"ok": True, "claim_token": "tok-1", "claim": {"entity_id": 6832}},
        )
        self.assertEqual(harness.observed_task_id(record), "6832")

    def test_task_id_falls_back_to_argv_entity_when_stdout_has_neither(self) -> None:
        record = observed_record(
            ts=1.0,
            seq=0,
            argv=["claim", "--entity", "task:6832"],
            stdout={"ok": True, "claim_token": "tok-1"},
        )
        self.assertEqual(harness.observed_task_id(record), "6832")

    def test_completion_recorded_before_review_is_rejected_from_order(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            observed_dir = root / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            # `complete` is timestamped BEFORE the reviewer boundary event
            # below -- the synthetic violation the audit-fallback logic
            # could not see (task.after.json/claims.after.json said the
            # task was done either way) but the observed order rejects.
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
                ),
            )
            events_path = root / "events.jsonl"
            events_path.write_text(
                json.dumps(
                    {"event": "coder-finished", "source": "controlled-coder", "ts": 1.5}
                )
                + "\n"
                + json.dumps(
                    {"event": "review-approved", "source": "controlled-reviewer", "ts": 3.0}
                )
                + "\n",
                encoding="utf-8",
            )
            events = harness.merge_lifecycle_events(observed_dir, events_path)
            names = [event.get("event") for event in events]
            self.assertEqual(
                names,
                ["claim-acquired", "coder-finished", "task-completed", "review-approved"],
            )
            artifact_dir = root / "artifacts"
            artifact_dir.mkdir()
            harness.write_json(artifact_dir / "events.normalized.json", events)
            harness.write_json(artifact_dir / "task.after.json", {"status": "done"})
            harness.write_json(artifact_dir / "claims.after.json", {"active": []})
            harness.write_text(artifact_dir / "fixture-test.txt", "PASS\n")
            harness.write_json(artifact_dir / "fixture-test.json", {"returncode": 0})
            harness.write_text(
                artifact_dir / "repo" / "src" / "value.txt", "approved\n"
            )
            with self.assertRaisesRegex(harness.EvalFailure, "ordered event missing"):
                harness.grade_lifecycle_artifacts(
                    lifecycle_case(), artifact_dir, lifecycle_options()
                )

    def test_two_terminal_verbs_on_one_claim_token_fail(self) -> None:
        records = [
            observed_record(
                ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
            ),
            observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            observed_record(
                    ts=3.0,
                    seq=2,
                    argv=["release", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
        ]
        with self.assertRaisesRegex(harness.EvalFailure, "tok-1"):
            harness.assert_terminal_verb_lifecycle(records)

    def test_exactly_one_terminal_verb_is_accepted(self) -> None:
        records = [
            observed_record(
                ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
            ),
            observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
        ]
        harness.assert_terminal_verb_lifecycle(records)

    def test_heartbeat_without_ttl_is_rejected(self) -> None:
        records = [
            observed_record(ts=1.0, seq=0, argv=["heartbeat", "--claim", "tok-1"]),
        ]
        with self.assertRaisesRegex(harness.EvalFailure, "heartbeat"):
            harness.assert_terminal_verb_lifecycle(records)

    def test_heartbeat_with_ttl_is_accepted(self) -> None:
        records = [
            observed_record(
                ts=1.0, seq=0, argv=["heartbeat", "--claim", "tok-1", "--ttl", "8h"]
            ),
        ]
        harness.assert_terminal_verb_lifecycle(records)

    def test_claim_after_terminal_verb_on_same_task_is_rejected(self) -> None:
        records = [
            observed_record(
                    ts=1.0,
                    seq=0,
                    argv=["pull"],
                    stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
                ),
            observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
                ),
            observed_record(
                    ts=3.0,
                    seq=2,
                    argv=["claim", "--entity", "task:6832"],
                    stdout=claim_stdout(claim_token="tok-2"),
                ),
        ]
        with self.assertRaisesRegex(harness.EvalFailure, "6832"):
            harness.assert_terminal_verb_lifecycle(records)

    def test_failed_terminal_verb_does_not_count_toward_one_per_token(self) -> None:
        # C4: a second `complete` that the real binary rejected (non-zero
        # exit) is retained in the observed log for diagnosis, but it must
        # not itself trip the "exactly one terminal verb" assertion --
        # only a forwarded success counts as a terminal verb having
        # happened. It IS surfaced in the returned terminal_attempt_failures
        # list rather than silently dropped.
        records = [
            observed_record(
                ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
            ),
            observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            observed_record(
                    ts=3.0,
                    seq=2,
                    argv=["complete", "--claim", "tok-1"],
                    exit_code=1,
                    stderr="already completed",
                ),
        ]
        failures = harness.assert_terminal_verb_lifecycle(records)
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["command"], "complete")
        self.assertEqual(failures[0]["claim_token"], "tok-1")
        self.assertEqual(failures[0]["exit_code"], 1)
        self.assertIn("already completed", failures[0]["stderr"])

    def test_release_then_claim_on_same_task_is_not_rejected(self) -> None:
        # C5: `release` is a hand-back the ritual explicitly allows to be
        # followed by a re-claim (lapsed-claim recovery: re-claim with
        # --no-transition then complete). Only complete|fail|block gate
        # "no claim/pull after a terminal verb on the same task".
        records = [
            observed_record(
                ts=1.0,
                seq=0,
                argv=["pull"],
                stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
            ),
            observed_record(
                ts=2.0,
                seq=1,
                argv=["release", "--claim", "tok-1"],
                stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
            ),
            observed_record(
                ts=3.0,
                seq=2,
                argv=["claim", "--entity", "task:6832"],
                stdout=claim_stdout(claim_token="tok-2", task_id="6832"),
            ),
        ]
        # No exception -- the post-release claim is legitimate recovery.
        harness.assert_terminal_verb_lifecycle(records)

    def test_claim_after_complete_on_same_task_is_still_rejected(self) -> None:
        # The C5 exemption is `release`-specific: complete|fail|block still
        # gate a later claim/pull on the same task.
        records = [
            observed_record(
                ts=1.0,
                seq=0,
                argv=["pull"],
                stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
            ),
            observed_record(
                ts=2.0,
                seq=1,
                argv=["complete", "--claim", "tok-1"],
                stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
            ),
            observed_record(
                ts=3.0,
                seq=2,
                argv=["claim", "--entity", "task:6832"],
                stdout=claim_stdout(claim_token="tok-2", task_id="6832"),
            ),
        ]
        with self.assertRaisesRegex(harness.EvalFailure, "6832"):
            harness.assert_terminal_verb_lifecycle(records)


def make_collect_context(repo: Path, artifacts: Path) -> harness.LifecycleContext:
    return harness.LifecycleContext(
        case_path=Path("<synthetic-case>"),
        case={"id": "synthetic-collect-probe"},
        artifacts=artifacts,
        repo=repo,
        env=dict(os.environ),
        plan_id="1",
        task_id="6832",
    )


def collect_options() -> harness.Options:
    return harness.Options(
        mode="lifecycle-fixture",
        vendor="",
        surface="agent",
        case_filter=None,
        results_dir=None,
        keep=True,
    )


class CollectLifecycleArtifactsWiringTest(unittest.TestCase):
    """`collect_lifecycle_artifacts` must actually CALL
    `assert_wrapper_not_bypassed` and `assert_terminal_verb_lifecycle`, not
    merely have them defined and unit-tested in isolation. `run_json` (the
    only thing collect uses to reach `planar`/`planar audit trail`) is
    mocked so this drives the real function over a synthetic repo tree with
    no `planar` binary involved -- the same wiring gap cycle 1 hit with
    `stage_vendor_config`.
    """

    def run_collect(
        self, repo: Path, artifacts: Path, audit_claims: list[dict[str, object]]
    ) -> None:
        def fake_run_json(args, *, cwd=None, env=None):
            if args[:2] == ["planar", "task"]:
                return {"status": "done"}
            if args[:2] == ["planar", "audit"]:
                return {"agent_activity": {"claims": audit_claims}}
            raise AssertionError(f"unexpected run_json call: {args}")

        context = make_collect_context(repo, artifacts)
        with mock.patch.object(harness, "run_json", side_effect=fake_run_json):
            harness.collect_lifecycle_artifacts(context, collect_options())

    def test_empty_observed_dir_with_audit_claims_raises_wrapper_bypassed(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            repo = root / "repo"
            (repo / ".eval").mkdir(parents=True)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            with self.assertRaisesRegex(harness.EvalFailure, "wrapper-bypassed"):
                self.run_collect(
                    repo, artifacts, audit_claims=[{"status": "completed"}]
                )

    def test_record_failed_sentinel_fails_the_run(self) -> None:
        # C2 wiring: collect_lifecycle_artifacts must actually call
        # assert_no_recording_failures, not just have it defined.
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            repo = root / "repo"
            observed_dir = repo / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            observed_dir.mkdir(parents=True, exist_ok=True)
            (observed_dir / ".record-failed").write_text(
                "seq=1 ts=2.0 exit=0 record_rc=9 argv=heartbeat\n", encoding="utf-8"
            )
            artifacts = root / "artifacts"
            artifacts.mkdir()
            with self.assertRaisesRegex(harness.EvalFailure, "recording failed"):
                self.run_collect(repo, artifacts, audit_claims=[])

    def test_terminal_attempt_failures_are_written_as_warnings(self) -> None:
        # C4 wiring: a failed second `complete` must not fail the run and
        # must be surfaced in the retained warnings.json, not silently
        # dropped.
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            repo = root / "repo"
            observed_dir = repo / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0,
                    seq=0,
                    argv=["pull"],
                    stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=3.0,
                    seq=2,
                    argv=["complete", "--claim", "tok-1"],
                    exit_code=1,
                    stderr="already completed",
                ),
            )
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (repo / "src").mkdir(parents=True)
            (repo / "src" / "value.txt").write_text("approved\n", encoding="utf-8")

            def fake_run_json(args, *, cwd=None, env=None):
                if args[:2] == ["planar", "task"]:
                    return {"status": "done"}
                if args[:2] == ["planar", "audit"]:
                    return {"agent_activity": {"claims": [{"status": "completed"}]}}
                if args[:2] == ["planar-watch", "ps"]:
                    return {"active": []}
                raise AssertionError(f"unexpected run_json call: {args}")

            def fake_run_command(args, *, cwd=None, env=None, check=True, timeout_seconds=None):
                if args[:2] == ["make", "test"]:
                    return subprocess.CompletedProcess(args, 0, "PASS\n", "")
                if args[0] == "git":
                    return subprocess.CompletedProcess(args, 0, "", "")
                raise AssertionError(f"unexpected run_command call: {args}")

            context = make_collect_context(repo, artifacts)
            with mock.patch.object(
                harness, "run_json", side_effect=fake_run_json
            ), mock.patch.object(
                harness, "run_command", side_effect=fake_run_command
            ):
                harness.collect_lifecycle_artifacts(context, collect_options())
            warnings = harness.read_json(artifacts / "warnings.json")
            failures = warnings["terminal_attempt_failures"]
            self.assertEqual(len(failures), 1)
            self.assertEqual(failures[0]["command"], "complete")
            self.assertEqual(failures[0]["exit_code"], 1)

    def test_two_terminal_verbs_in_observed_dir_fails_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            repo = root / "repo"
            observed_dir = repo / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=3.0,
                    seq=2,
                    argv=["release", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            artifacts = root / "artifacts"
            artifacts.mkdir()
            with self.assertRaisesRegex(harness.EvalFailure, "tok-1"):
                self.run_collect(repo, artifacts, audit_claims=[])

    def test_clean_observed_dir_does_not_raise(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            repo = root / "repo"
            observed_dir = repo / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0,
                    seq=0,
                    argv=["pull"],
                    stdout=pull_stdout(claim_token="tok-1", task_id="6832"),
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1", task_id="6832"),
                ),
            )
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (repo / "src").mkdir(parents=True)
            (repo / "src" / "value.txt").write_text("approved\n", encoding="utf-8")

            def fake_run_json(args, *, cwd=None, env=None):
                if args[:2] == ["planar", "task"]:
                    return {"status": "done"}
                if args[:2] == ["planar", "audit"]:
                    return {"agent_activity": {"claims": [{"status": "completed"}]}}
                if args[:2] == ["planar-watch", "ps"]:
                    return {"active": []}
                raise AssertionError(f"unexpected run_json call: {args}")

            def fake_run_command(args, *, cwd=None, env=None, check=True, timeout_seconds=None):
                if args[:2] == ["make", "test"]:
                    return subprocess.CompletedProcess(args, 0, "PASS\n", "")
                if args[:2] == ["git", "status"]:
                    return subprocess.CompletedProcess(args, 0, "", "")
                if args[0] == "git":
                    return subprocess.CompletedProcess(args, 0, "", "")
                raise AssertionError(f"unexpected run_command call: {args}")

            context = make_collect_context(repo, artifacts)
            with mock.patch.object(
                harness, "run_json", side_effect=fake_run_json
            ), mock.patch.object(
                harness, "run_command", side_effect=fake_run_command
            ):
                harness.collect_lifecycle_artifacts(context, collect_options())
            events = harness.read_json(artifacts / "events.normalized.json")
            self.assertEqual(
                [e.get("event") for e in events], ["claim-acquired", "task-completed"]
            )


LIFECYCLE_CASE_REQUIRED_FIELDS: dict[str, object] = {
    "schema_version": 1,
    "skill": "orchestrator",
    "description": "probe",
    "tags": ["lifecycle"],
    "tiers": ["contract", "lifecycle"],
    "tasks": [{"slug": "controlled-value", "title": "t"}],
    "contract_assertions": [
        {
            "id": "probe",
            "description": "probe",
            "mode": "all",
            "paths": ["agents/methodology.md"],
            "pattern": r"\bthe\b",
        }
    ],
    "setup": {"fixture": "controlled-classic", "specialist_scenario": "success"},
    "lifecycle": {
        "scenario": "controlled-classic",
        "surface": "agent",
        "prompt": "probe",
    },
}


def minimal_valid_lifecycle_case(case_id: str) -> dict[str, object]:
    case = dict(LIFECYCLE_CASE_REQUIRED_FIELDS)
    case["id"] = case_id
    case["expected"] = {
        "post_state": {"task_status": "done", "file_value": "approved"}
    }
    return case


class RequiredPostStateTests(unittest.TestCase):
    """Task 6837 (D4): `expected.post_state.task_status` and `file_value` are
    REQUIRED on every lifecycle-tier case; the grader no longer has a
    silent-skip branch for either being absent.
    """

    def test_missing_task_status_fails_validation_naming_the_key(self) -> None:
        case = minimal_valid_lifecycle_case("missing-task-status")
        case["expected"] = {"post_state": {"file_value": "approved"}}
        with self.assertRaisesRegex(harness.EvalFailure, "task_status"):
            harness.validate_case(Path("<probe>"), case)

    def test_missing_file_value_fails_validation_naming_the_key(self) -> None:
        case = minimal_valid_lifecycle_case("missing-file-value")
        case["expected"] = {"post_state": {"task_status": "done"}}
        with self.assertRaisesRegex(harness.EvalFailure, "file_value"):
            harness.validate_case(Path("<probe>"), case)

    def test_missing_post_state_entirely_fails_validation(self) -> None:
        case = minimal_valid_lifecycle_case("missing-post-state")
        case["expected"] = {}
        with self.assertRaisesRegex(harness.EvalFailure, "post_state"):
            harness.validate_case(Path("<probe>"), case)

    def test_unregistered_replay_driver_fails_validation(self) -> None:
        case = minimal_valid_lifecycle_case("bad-replay-driver")
        case["setup"] = dict(case["setup"])
        case["setup"]["replay"] = "not-a-real-driver"
        with self.assertRaisesRegex(harness.EvalFailure, "not-a-real-driver"):
            harness.validate_case(Path("<probe>"), case)

    def test_iteration_cap_must_be_a_positive_integer(self) -> None:
        # Task 6856: setup.iteration_cap is type-checked whenever a case
        # declares it -- CALL-SITE test for validate_case's own check
        # (a no-op there would let a string, a bool, zero, or a negative
        # value silently reach replay_driver_iteration_cap).
        for bad_cap in (0, -1, "5", 5.0, True):
            case = minimal_valid_lifecycle_case("bad-iteration-cap")
            case["setup"] = dict(case["setup"])
            case["setup"]["iteration_cap"] = bad_cap
            with self.assertRaisesRegex(
                harness.EvalFailure, "setup.iteration_cap must be a positive integer"
            ):
                harness.validate_case(Path("<probe>"), case)

    def test_iteration_cap_absent_is_valid(self) -> None:
        case = minimal_valid_lifecycle_case("no-iteration-cap")
        harness.validate_case(Path("<probe>"), case)

    def test_iteration_cap_positive_integer_is_valid(self) -> None:
        case = minimal_valid_lifecycle_case("good-iteration-cap")
        case["setup"] = dict(case["setup"])
        case["setup"]["iteration_cap"] = 5
        harness.validate_case(Path("<probe>"), case)

    def test_claim_rows_must_be_a_non_negative_integer(self) -> None:
        # Task 6855: post_state.claim_rows is type-checked whenever a case
        # declares it -- CALL-SITE test for validate_case's own check.
        for bad_rows in (-1, "2", 2.0, True):
            case = minimal_valid_lifecycle_case("bad-claim-rows")
            case["expected"] = dict(case["expected"])
            case["expected"]["post_state"] = dict(case["expected"]["post_state"])
            case["expected"]["post_state"]["claim_rows"] = bad_rows
            with self.assertRaisesRegex(
                harness.EvalFailure,
                "expected.post_state.claim_rows must be a non-negative integer",
            ):
                harness.validate_case(Path("<probe>"), case)

    def test_claim_rows_absent_is_valid(self) -> None:
        case = minimal_valid_lifecycle_case("no-claim-rows")
        harness.validate_case(Path("<probe>"), case)

    def test_claim_rows_zero_is_valid(self) -> None:
        # 0 must be accepted, not treated as falsy-and-therefore-absent --
        # mirrors the existing active_claims contract (grade time reads it
        # with `is not None`, not truthiness).
        case = minimal_valid_lifecycle_case("zero-claim-rows")
        case["expected"] = dict(case["expected"])
        case["expected"]["post_state"] = dict(case["expected"]["post_state"])
        case["expected"]["post_state"]["claim_rows"] = 0
        harness.validate_case(Path("<probe>"), case)

    def test_duplicate_task_slug_fails_validation(self) -> None:
        case = minimal_valid_lifecycle_case("dup-slug-probe")
        case["tasks"] = [
            {"slug": "same-slug", "title": "one"},
            {"slug": "same-slug", "title": "two"},
        ]
        with self.assertRaisesRegex(harness.EvalFailure, "duplicate lifecycle task slug"):
            harness.validate_case(Path("<probe>"), case)

    def test_depends_on_unknown_slug_fails_validation(self) -> None:
        case = minimal_valid_lifecycle_case("bad-depends-on-probe")
        case["tasks"] = [
            {"slug": "task-one", "title": "one", "depends_on": "does-not-exist"},
        ]
        with self.assertRaisesRegex(harness.EvalFailure, "depends_on"):
            harness.validate_case(Path("<probe>"), case)

    def test_a_non_lifecycle_case_is_unaffected(self) -> None:
        case = {
            "schema_version": 1,
            "id": "contract-only-probe",
            "skill": "orchestrator",
            "description": "probe",
            "tags": ["contract"],
            "tiers": ["contract"],
            "contract_assertions": [
                {
                    "id": "probe",
                    "description": "probe",
                    "mode": "all",
                    "paths": ["agents/methodology.md"],
                    "pattern": r"\bthe\b",
                }
            ],
            "expected": {},
        }
        # Must not raise: a contract-only case declares no post_state at all.
        harness.validate_case(Path("<probe>"), case)

    def test_load_cases_rejects_an_on_disk_case_missing_post_state_keys(self) -> None:
        # Drives the real caller (`load_cases`) rather than `validate_case`
        # directly, so a no-op at either the `load_cases` -> `validate_case`
        # call site or the check inside `validate_case` fails this test.
        case = minimal_valid_lifecycle_case("on-disk-missing-post-state-key")
        case["expected"] = {"post_state": {"task_status": "done"}}
        with tempfile.TemporaryDirectory() as raw_tmp:
            cases_dir = Path(raw_tmp)
            (cases_dir / "on-disk-missing-post-state-key.json").write_text(
                json.dumps(case), encoding="utf-8"
            )
            with mock.patch.object(harness, "CASES_DIR", cases_dir):
                with self.assertRaisesRegex(harness.EvalFailure, "file_value"):
                    harness.load_cases()

    def test_existing_lifecycle_cases_declare_both_keys(self) -> None:
        # Task 6854/6857: a lifecycle case declares EXACTLY ONE of
        # task_status (single-task cases) or tasks (multi-task cases),
        # plus file_value unconditionally, in every repository case.
        for path, case in harness.load_cases():
            if "lifecycle" not in case["tiers"]:
                continue
            post_state = case["expected"]["post_state"]
            self.assertTrue(
                bool(post_state.get("task_status")) != bool(post_state.get("tasks")),
                path,
            )
            self.assertTrue(post_state.get("file_value"), path)

    def test_grader_no_longer_skips_a_task_status_mismatch(self) -> None:
        # Before task 6837 the grader's `if expected_status and ...` guard
        # meant a case whose expected task_status was falsy silently
        # accepted ANY actual status. That shape can no longer reach the
        # grader (validate_case now refuses it), so this proves the
        # grader's own comparison is unconditional given a real value: a
        # real mismatch is still caught.
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.write_json(artifact_dir / "task.after.json", {"status": "blocked"})
            with self.assertRaisesRegex(
                harness.EvalFailure, "task status was blocked, expected done"
            ):
                harness.grade_lifecycle_artifacts(
                    lifecycle_case(), artifact_dir, lifecycle_options()
                )


class MultiTaskPostStateGradingTests(unittest.TestCase):
    """Task 6857: `expected.post_state.tasks` (a list of {slug, status})
    is an alternative to the single-task `task_status` key.
    `grade_lifecycle_artifacts` must read `tasks.after.json` and check
    EVERY listed task, not just fall through and silently ignore `tasks`
    in favor of nothing.
    """

    def multi_task_case(self) -> dict[str, object]:
        case = lifecycle_case()
        case["expected"] = dict(case["expected"])
        case["expected"]["post_state"] = {
            "tasks": [
                {"slug": "blocker-task", "status": "done"},
                {"slug": "dependent-task", "status": "done"},
            ],
            "active_claims": 0,
            "file_value": "approved",
        }
        return case

    def write_tasks_after(self, artifact_dir: Path, statuses: dict[str, str]) -> None:
        harness.write_json(
            artifact_dir / "tasks.after.json",
            [{"slug": slug, "status": status} for slug, status in statuses.items()],
        )

    def test_call_site_checks_every_listed_task(self) -> None:
        # CALL-SITE test: a mutant that reads only post_state["tasks"][0],
        # or that skips the "tasks" branch entirely and falls through to
        # task.after.json, must fail this test -- the blocker is "done"
        # (so a task.after.json-only check would pass) but the dependent
        # is still "doing".
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            self.write_tasks_after(
                artifact_dir, {"blocker-task": "done", "dependent-task": "doing"}
            )
            with self.assertRaisesRegex(
                harness.EvalFailure,
                "task dependent-task status was doing, expected done",
            ):
                harness.grade_lifecycle_artifacts(
                    self.multi_task_case(), artifact_dir, lifecycle_options()
                )

    def test_all_tasks_matching_passes(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            self.write_tasks_after(
                artifact_dir, {"blocker-task": "done", "dependent-task": "done"}
            )
            harness.grade_lifecycle_artifacts(
                self.multi_task_case(), artifact_dir, lifecycle_options()
            )

    def test_missing_tasks_after_json_fails_naming_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            with self.assertRaisesRegex(harness.EvalFailure, "tasks.after.json"):
                harness.grade_lifecycle_artifacts(
                    self.multi_task_case(), artifact_dir, lifecycle_options()
                )


class ClaimRowsPostStateGradingTests(unittest.TestCase):
    """Task 6855: `expected.post_state.claim_rows` grades the TOTAL number
    of `agent_work_claims` rows the task accumulated (`task.audit.json`'s
    `agent_activity.claims`), distinguishing a lapsed-claim recovery (two
    rows: the lapsed pull, the --no-transition recovery claim) from a case
    that only ever claimed once.
    """

    def claim_rows_case(self, expected_rows: int) -> dict[str, object]:
        case = lifecycle_case()
        case["expected"] = dict(case["expected"])
        case["expected"]["post_state"] = dict(case["expected"]["post_state"])
        case["expected"]["post_state"]["claim_rows"] = expected_rows
        return case

    def write_audit(self, artifact_dir: Path, claim_count: int) -> None:
        harness.write_json(
            artifact_dir / "task.audit.json",
            {
                "agent_activity": {
                    "actions": [],
                    "claims": [
                        {
                            "id": index,
                            "claim_token": f"tok-{index}",
                            "status": "released" if index == 0 else "completed",
                        }
                        for index in range(claim_count)
                    ],
                }
            },
        )

    def test_call_site_checks_the_real_audit_claim_count(self) -> None:
        # CALL-SITE test: a mutant that always reads len(claims) == 1, or
        # that no-ops the whole claim_rows branch, must fail this test --
        # the case declares 2, the retained audit only has 1.
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            self.write_audit(artifact_dir, 1)
            with self.assertRaisesRegex(
                harness.EvalFailure, "claim row count was 1, expected 2"
            ):
                harness.grade_lifecycle_artifacts(
                    self.claim_rows_case(2), artifact_dir, lifecycle_options()
                )

    def test_matching_claim_rows_passes(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            self.write_audit(artifact_dir, 2)
            harness.grade_lifecycle_artifacts(
                self.claim_rows_case(2), artifact_dir, lifecycle_options()
            )

    def test_missing_task_audit_json_fails_naming_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            with self.assertRaisesRegex(
                harness.EvalFailure, "task.audit.json"
            ):
                harness.grade_lifecycle_artifacts(
                    self.claim_rows_case(2), artifact_dir, lifecycle_options()
                )

    def test_absent_claim_rows_key_skips_the_check(self) -> None:
        # A case that never declares claim_rows must not be affected by
        # this branch at all -- no task.audit.json needed.
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.grade_lifecycle_artifacts(
                lifecycle_case(), artifact_dir, lifecycle_options()
            )


class ReplayDriverRegistryTests(unittest.TestCase):
    """Task 6854/6857: `run_lifecycle_fixture_replay` dispatches to the
    driver named by `setup.replay` (default `classic`), and every driver
    still goes through the shared collect/grade wrapper.
    """

    def test_call_site_dispatches_to_the_named_driver(self) -> None:
        # CALL-SITE test: a mutant that hardcodes replay_driver_classic (or
        # otherwise ignores setup.replay) must fail this test.
        case = {
            "id": "driver-dispatch-probe",
            "setup": {"replay": "session-death"},
        }
        fake_context = harness.LifecycleContext(
            case_path=Path("<probe>"),
            case=case,
            artifacts=Path("<artifacts>"),
            repo=Path("<repo>"),
            env={},
            plan_id="1",
            task_id="1",
            task_ids=["1"],
        )
        with mock.patch.object(
            harness, "prepare_lifecycle_fixture", return_value=fake_context
        ), mock.patch.object(
            harness, "replay_driver_classic"
        ) as classic_mock, mock.patch.object(
            harness, "replay_driver_session_death"
        ) as session_death_mock, mock.patch.object(
            harness, "collect_lifecycle_artifacts"
        ), mock.patch.object(
            harness, "grade_lifecycle_artifacts"
        ), mock.patch.object(
            harness, "write_grade"
        ), mock.patch.object(
            harness, "finish_artifacts"
        ):
            # `REPLAY_DRIVERS` closes over the ORIGINAL function objects at
            # module-import time, so patching the module attribute alone
            # does not redirect a dict lookup made against the stale copy.
            # Patch the registry entry too, mirroring how `main()` would
            # observe a real code change.
            with mock.patch.dict(
                harness.REPLAY_DRIVERS,
                {"session-death": session_death_mock, "classic": classic_mock},
            ):
                harness.run_lifecycle_fixture_replay(
                    Path("<case-path>"), case, lifecycle_options()
                )
        session_death_mock.assert_called_once()
        classic_mock.assert_not_called()

    def test_call_site_dispatches_lapsed_claim_and_iteration_cap(self) -> None:
        # CALL-SITE test for the two task-6855/6856 drivers: a mutant that
        # forgets to register either in REPLAY_DRIVERS, or that dispatches
        # to the wrong driver for its name, must fail this test.
        for replay_name, driver_attr, other_attrs in (
            (
                "lapsed-claim",
                "replay_driver_lapsed_claim",
                ("replay_driver_classic", "replay_driver_iteration_cap"),
            ),
            (
                "iteration-cap",
                "replay_driver_iteration_cap",
                ("replay_driver_classic", "replay_driver_lapsed_claim"),
            ),
        ):
            case = {
                "id": f"driver-dispatch-probe-{replay_name}",
                "setup": {"replay": replay_name},
            }
            fake_context = harness.LifecycleContext(
                case_path=Path("<probe>"),
                case=case,
                artifacts=Path("<artifacts>"),
                repo=Path("<repo>"),
                env={},
                plan_id="1",
                task_id="1",
                task_ids=["1"],
            )
            with mock.patch.object(
                harness, "prepare_lifecycle_fixture", return_value=fake_context
            ), mock.patch.object(harness, driver_attr) as wanted_mock, mock.patch.object(
                harness, other_attrs[0]
            ) as other_mock_a, mock.patch.object(
                harness, other_attrs[1]
            ) as other_mock_b, mock.patch.object(
                harness, "collect_lifecycle_artifacts"
            ), mock.patch.object(
                harness, "grade_lifecycle_artifacts"
            ), mock.patch.object(
                harness, "write_grade"
            ), mock.patch.object(
                harness, "finish_artifacts"
            ):
                with mock.patch.dict(
                    harness.REPLAY_DRIVERS,
                    {
                        replay_name: wanted_mock,
                        other_attrs[0].removeprefix("replay_driver_").replace(
                            "_", "-"
                        ): other_mock_a,
                        other_attrs[1].removeprefix("replay_driver_").replace(
                            "_", "-"
                        ): other_mock_b,
                    },
                ):
                    harness.run_lifecycle_fixture_replay(
                        Path("<case-path>"), case, lifecycle_options()
                    )
            wanted_mock.assert_called_once()
            other_mock_a.assert_not_called()
            other_mock_b.assert_not_called()

    def test_unregistered_driver_fails_before_any_replay_work(self) -> None:
        case = {
            "id": "driver-unregistered-probe",
            "setup": {"replay": "not-a-real-driver"},
        }
        fake_context = harness.LifecycleContext(
            case_path=Path("<probe>"),
            case=case,
            artifacts=Path("<artifacts>"),
            repo=Path("<repo>"),
            env={},
            plan_id="1",
            task_id="1",
            task_ids=["1"],
        )
        with mock.patch.object(
            harness, "prepare_lifecycle_fixture", return_value=fake_context
        ), mock.patch.object(harness, "write_grade") as write_grade_mock:
            with self.assertRaisesRegex(
                harness.EvalFailure, "unknown lifecycle replay driver"
            ):
                harness.run_lifecycle_fixture_replay(
                    Path("<case-path>"), case, lifecycle_options()
                )
        # `live_failure` writes a grade.json to `context.artifacts` on this
        # failure path. `context.artifacts` here is the placeholder
        # `Path("<artifacts>")`, not a real tmpdir -- left unmocked, this
        # test used to create a real directory literally named `<artifacts>`
        # under the repo root's cwd every run (task 6917). `write_grade` is
        # mocked above instead of giving the fixture a real tmp path, since
        # this test's whole point is that dispatch never reaches replay
        # work, not that grade-writing behaves a particular way.
        write_grade_mock.assert_called_once()
        self.assertEqual(write_grade_mock.call_args.args[0], Path("<artifacts>"))


class ConcurrentCodersGuardTests(unittest.TestCase):
    """Task 6853: the three new guards `replay_driver_concurrent_coders`
    calls -- distinctness of concurrent claims, no busy/QueryFailed pull
    exit, the fan-in merge preceding every complete, and the epic branch
    actually containing every lane commit. Each guard gets a CALL-SITE
    test: a mutant that no-ops the guard's check must fail the test.
    """

    def lane_claims(self) -> list[dict[str, object]]:
        return [
            {"lane": 1, "claim_token": "tok-1", "task_id": "11"},
            {"lane": 2, "claim_token": "tok-2", "task_id": "12"},
            {"lane": 3, "claim_token": "tok-3", "task_id": "13"},
        ]

    # -- assert_distinct_lane_claims -------------------------------------

    def test_call_site_detects_duplicate_claim_tokens(self) -> None:
        claims = self.lane_claims()
        claims[1]["claim_token"] = claims[0]["claim_token"]
        with self.assertRaisesRegex(
            harness.EvalFailure, "duplicate claim tokens"
        ):
            harness.assert_distinct_lane_claims(
                claims, artifacts=Path("<artifacts>"), case_id="probe",
                options=lifecycle_options(),
            )

    def test_call_site_detects_duplicate_claimed_tasks(self) -> None:
        # CALL-SITE test: a mutant that checks only tokens (or that
        # no-ops the whole function) must fail this test -- two DISTINCT
        # claim tokens on the SAME task id is the real US-7 race, not a
        # token collision.
        claims = self.lane_claims()
        claims[1]["task_id"] = claims[0]["task_id"]
        with self.assertRaisesRegex(
            harness.EvalFailure, "duplicate claimed tasks"
        ):
            harness.assert_distinct_lane_claims(
                claims, artifacts=Path("<artifacts>"), case_id="probe",
                options=lifecycle_options(),
            )

    def test_distinct_claims_pass(self) -> None:
        harness.assert_distinct_lane_claims(
            self.lane_claims(), artifacts=Path("<artifacts>"), case_id="probe",
            options=lifecycle_options(),
        )

    # -- assert_no_concurrent_pull_failures -------------------------------

    def test_call_site_detects_a_failed_concurrent_pull(self) -> None:
        # CALL-SITE test: a mutant that ignores returncode (e.g. always
        # treats the batch as clean) must fail this test.
        ok = subprocess.CompletedProcess(args=[], returncode=0, stdout="{}", stderr="")
        busy = subprocess.CompletedProcess(
            args=[], returncode=1, stdout="", stderr="Error: QueryFailed: database is locked"
        )
        with self.assertRaisesRegex(harness.EvalFailure, "QueryFailed"):
            harness.assert_no_concurrent_pull_failures(
                [(1, ok), (2, busy), (3, ok)],
                artifacts=Path("<artifacts>"), case_id="probe",
                options=lifecycle_options(),
            )

    def test_all_pulls_succeeding_passes(self) -> None:
        ok = subprocess.CompletedProcess(args=[], returncode=0, stdout="{}", stderr="")
        harness.assert_no_concurrent_pull_failures(
            [(1, ok), (2, ok), (3, ok)],
            artifacts=Path("<artifacts>"), case_id="probe",
            options=lifecycle_options(),
        )

    # -- assert_merge_precedes_completes -----------------------------------

    def test_call_site_detects_a_complete_before_the_merge(self) -> None:
        # CALL-SITE test: a mutant that compares against the wrong
        # timestamp, or that skips this check entirely, must fail this
        # test -- tok-2's complete is recorded BEFORE merge_ts.
        records = [
            observed_record(
                ts=100.0, seq=0, argv=["complete", "--claim", "tok-1"],
                stdout=terminal_stdout(claim_token="tok-1"),
            ),
            observed_record(
                ts=50.0, seq=1, argv=["complete", "--claim", "tok-2"],
                stdout=terminal_stdout(claim_token="tok-2"),
            ),
            observed_record(
                ts=101.0, seq=2, argv=["complete", "--claim", "tok-3"],
                stdout=terminal_stdout(claim_token="tok-3"),
            ),
        ]
        with self.assertRaisesRegex(
            harness.EvalFailure, "complete observed before the fan-in merge"
        ):
            harness.assert_merge_precedes_completes(
                60.0, records, ["tok-1", "tok-2", "tok-3"],
                artifacts=Path("<artifacts>"), case_id="probe",
                options=lifecycle_options(),
            )

    def test_call_site_detects_a_missing_complete_record(self) -> None:
        records = [
            observed_record(
                ts=100.0, seq=0, argv=["complete", "--claim", "tok-1"],
                stdout=terminal_stdout(claim_token="tok-1"),
            ),
        ]
        with self.assertRaisesRegex(
            harness.EvalFailure, "no successful complete record"
        ):
            harness.assert_merge_precedes_completes(
                60.0, records, ["tok-1", "tok-2"],
                artifacts=Path("<artifacts>"), case_id="probe",
                options=lifecycle_options(),
            )

    def test_all_completes_after_the_merge_pass(self) -> None:
        records = [
            observed_record(
                ts=100.0, seq=0, argv=["complete", "--claim", "tok-1"],
                stdout=terminal_stdout(claim_token="tok-1"),
            ),
            observed_record(
                ts=101.0, seq=1, argv=["complete", "--claim", "tok-2"],
                stdout=terminal_stdout(claim_token="tok-2"),
            ),
        ]
        harness.assert_merge_precedes_completes(
            60.0, records, ["tok-1", "tok-2"],
            artifacts=Path("<artifacts>"), case_id="probe",
            options=lifecycle_options(),
        )

    # -- assert_epic_contains_lane_commits ---------------------------------

    def make_lane_and_epic_repo(self, tmp: Path) -> tuple[dict[str, str], str, str]:
        """A real 3-lane fan-in in a throwaway git repo -- returns
        (lane_shas, epic_sha, two_lane_epic_sha), the latter being the
        epic ref BEFORE the third lane was merged in, i.e. a real epic
        branch that genuinely omits one lane's commit."""
        env = dict(os.environ)
        harness.run_command(["git", "init", "-q"], cwd=tmp, env=env)
        harness.run_command(["git", "config", "user.name", "t"], cwd=tmp, env=env)
        harness.run_command(["git", "config", "user.email", "t@t"], cwd=tmp, env=env)
        (tmp / "value.txt").write_text("baseline\n", encoding="utf-8")
        harness.run_command(["git", "add", "-A"], cwd=tmp, env=env)
        harness.run_command(["git", "commit", "-q", "-m", "base"], cwd=tmp, env=env)
        base = harness.run_command(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=tmp, env=env
        ).stdout.strip()
        lane_shas: dict[str, str] = {}
        for lane in (1, 2, 3):
            harness.run_command(
                ["git", "checkout", "-q", "-b", f"lane-{lane}", base], cwd=tmp, env=env
            )
            (tmp / f"lane-{lane}.txt").write_text("approved\n", encoding="utf-8")
            harness.run_command(["git", "add", "-A"], cwd=tmp, env=env)
            harness.run_command(
                ["git", "commit", "-q", "-m", f"lane {lane}"], cwd=tmp, env=env
            )
            lane_shas[f"lane-{lane}"] = harness.run_command(
                ["git", "rev-parse", "HEAD"], cwd=tmp, env=env
            ).stdout.strip()
        harness.run_command(
            ["git", "checkout", "-q", "-b", "epic-fanin", base], cwd=tmp, env=env
        )
        harness.run_command(
            ["git", "merge", "-q", "--no-ff", "lane-1", "-m", "fan in lane-1"],
            cwd=tmp, env=env,
        )
        harness.run_command(
            ["git", "merge", "-q", "--no-ff", "lane-2", "-m", "fan in lane-2"],
            cwd=tmp, env=env,
        )
        two_lane_epic_sha = harness.run_command(
            ["git", "rev-parse", "HEAD"], cwd=tmp, env=env
        ).stdout.strip()
        harness.run_command(
            ["git", "merge", "-q", "--no-ff", "lane-3", "-m", "fan in lane-3"],
            cwd=tmp, env=env,
        )
        epic_sha = harness.run_command(
            ["git", "rev-parse", "HEAD"], cwd=tmp, env=env
        ).stdout.strip()
        return lane_shas, epic_sha, two_lane_epic_sha

    def test_call_site_detects_a_lane_missing_from_the_epic_branch(self) -> None:
        # CALL-SITE test: a mutant that trusts `git merge`'s own exit code
        # (which fails OPEN on a missing branch -- "Already up to date",
        # exit 0) rather than checking ancestry must fail this test. The
        # epic ref here is real: it is the fan-in commit BEFORE lane-3 was
        # merged, so lane-3's commit genuinely is not its ancestor.
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            lane_shas, _epic_sha, two_lane_epic_sha = self.make_lane_and_epic_repo(tmp)
            with self.assertRaisesRegex(
                harness.EvalFailure,
                "epic branch does not contain lane commit.*lane-3",
            ):
                harness.assert_epic_contains_lane_commits(
                    tmp, dict(os.environ), two_lane_epic_sha, lane_shas,
                    artifacts=Path("<artifacts>"), case_id="probe",
                    options=lifecycle_options(),
                )

    def test_epic_branch_containing_every_lane_passes(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            lane_shas, epic_sha, _two_lane_epic_sha = self.make_lane_and_epic_repo(tmp)
            harness.assert_epic_contains_lane_commits(
                tmp, dict(os.environ), epic_sha, lane_shas,
                artifacts=Path("<artifacts>"), case_id="probe",
                options=lifecycle_options(),
            )

    # -- REPLAY_DRIVERS dispatch --------------------------------------------

    def test_call_site_dispatches_concurrent_coders(self) -> None:
        # CALL-SITE test: a mutant that forgets to register
        # "concurrent-coders" in REPLAY_DRIVERS, or that dispatches to the
        # wrong driver for its name, must fail this test.
        case = {"id": "driver-dispatch-probe-concurrent-coders", "setup": {"replay": "concurrent-coders"}}
        fake_context = harness.LifecycleContext(
            case_path=Path("<probe>"),
            case=case,
            artifacts=Path("<artifacts>"),
            repo=Path("<repo>"),
            env={},
            plan_id="1",
            task_id="1",
            task_ids=["1", "2", "3"],
        )
        with mock.patch.object(
            harness, "prepare_lifecycle_fixture", return_value=fake_context
        ), mock.patch.object(
            harness, "replay_driver_concurrent_coders"
        ) as wanted_mock, mock.patch.object(
            harness, "replay_driver_classic"
        ) as other_mock, mock.patch.object(
            harness, "collect_lifecycle_artifacts"
        ), mock.patch.object(
            harness, "grade_lifecycle_artifacts"
        ), mock.patch.object(
            harness, "write_grade"
        ), mock.patch.object(
            harness, "finish_artifacts"
        ):
            with mock.patch.dict(
                harness.REPLAY_DRIVERS,
                {"concurrent-coders": wanted_mock, "classic": other_mock},
            ):
                harness.run_lifecycle_fixture_replay(
                    Path("<case-path>"), case, lifecycle_options()
                )
        wanted_mock.assert_called_once()
        other_mock.assert_not_called()


class RegradeCatchesRetainedObservedLogTest(unittest.TestCase):
    """`grade_lifecycle_artifacts` (and therefore `regrade_artifacts`, which
    calls it on a retained artifact dir) must re-derive the lifecycle
    invariants from the RETAINED `repo/.eval/observed` tree rather than
    trusting whatever `events.normalized.json` says -- otherwise
    `--grade-artifacts` on a retained run silently skips the checks
    collect-time already enforced.
    """

    def test_regrade_of_a_two_terminal_verb_artifact_fails(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            observed_dir = artifact_dir / "repo" / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=3.0,
                    seq=2,
                    argv=["release", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            with self.assertRaisesRegex(harness.EvalFailure, "tok-1"):
                harness.grade_lifecycle_artifacts(
                    lifecycle_case(), artifact_dir, lifecycle_options()
                )

    def test_regrade_of_a_clean_observed_log_still_passes(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            observed_dir = artifact_dir / "repo" / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            harness.grade_lifecycle_artifacts(
                lifecycle_case(), artifact_dir, lifecycle_options()
            )


class ArtifactFormatObservedLogGateTest(unittest.TestCase):
    """Task 6876: a missing `repo/.eval/observed` must fail grading when
    `run.json` declares `artifact_format >= ARTIFACT_FORMAT_OBSERVED_LOG`,
    and stay a legacy no-op when it doesn't (or when run.json is absent).

    Mutant: dropping the format check (always treating a missing
    observed/ as a legacy skip) must fail
    `test_missing_observed_log_fails_when_format_declares_it`.
    """

    def test_missing_observed_log_fails_when_format_declares_it(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.write_json(
                artifact_dir / "run.json",
                {"artifact_format": harness.ARTIFACT_FORMAT_OBSERVED_LOG},
            )
            # No repo/.eval/observed tree written: format >= 2 promises one.
            with self.assertRaisesRegex(
                harness.EvalFailure, "observed-log-missing"
            ):
                harness.grade_lifecycle_artifacts(
                    lifecycle_case(), artifact_dir, lifecycle_options()
                )

    def test_missing_observed_log_is_a_legacy_skip_below_the_format_floor(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.write_json(
                artifact_dir / "run.json",
                {"artifact_format": harness.ARTIFACT_FORMAT_OBSERVED_LOG - 1},
            )
            # Must not raise: a legacy artifact set never promised the tree.
            harness.grade_lifecycle_artifacts(
                lifecycle_case(), artifact_dir, lifecycle_options()
            )

    def test_missing_observed_log_is_a_legacy_skip_with_no_run_json(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            # No run.json at all -- predates the artifact_format key entirely.
            harness.grade_lifecycle_artifacts(
                lifecycle_case(), artifact_dir, lifecycle_options()
            )

    def test_present_observed_log_still_grades_normally_under_the_new_format(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            artifact_dir = Path(raw_tmp)
            write_lifecycle_artifacts(artifact_dir)
            harness.write_json(
                artifact_dir / "run.json",
                {"artifact_format": harness.ARTIFACT_FORMAT_OBSERVED_LOG},
            )
            observed_dir = artifact_dir / "repo" / ".eval" / "observed"
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=1.0, seq=0, argv=["pull"], stdout=pull_stdout(claim_token="tok-1")
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=2.0,
                    seq=1,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            harness.grade_lifecycle_artifacts(
                lifecycle_case(), artifact_dir, lifecycle_options()
            )

    def test_prepare_lifecycle_fixture_writes_the_observed_log_format_marker(
        self,
    ) -> None:
        # collection-time: run.json must carry artifact_format so grading
        # can tell a post-C2 run apart from a legacy one.
        case = {
            "id": "artifact-format-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "setup": {
                "fixture": "controlled-classic",
                "specialist_scenario": "baseline",
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-format-marker-") as results_root:
            options = harness.Options(
                mode="lifecycle-fixture",
                vendor="",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            context = harness.prepare_lifecycle_fixture(
                harness.ROOT / "evals" / "orchestrator" / "cases" / "classic-lifecycle-success.json",
                case,
                options,
                "fixture",
            )
            run_json = harness.read_json(context.artifacts / "run.json")
            self.assertEqual(
                run_json.get("artifact_format"), harness.ARTIFACT_FORMAT_OBSERVED_LOG
            )

    def test_prepare_lifecycle_fixture_creates_every_task_and_wires_depends_on(
        self,
    ) -> None:
        # CALL-SITE test for the multi-task loop in prepare_lifecycle_fixture
        # (task 6854/6857): a mutant that only creates case["tasks"][0], or
        # that drops the `planar task link ... --relationship depends-on`
        # call for a task carrying `depends_on`, must fail this test. Drives
        # the real function against the real `planar` binary (options.vendor
        # == "" -- fixture-replay mode, no vendor host or auth needed), the
        # same shape as the format-marker test above.
        case = {
            "id": "multi-task-prepare-probe",
            "tasks": [
                {"slug": "blocker-task", "title": "Blocker task"},
                {
                    "slug": "dependent-task",
                    "title": "Dependent task",
                    "depends_on": "blocker-task",
                },
            ],
            "setup": {
                "fixture": "controlled-classic",
                "specialist_scenario": "success",
            },
        }
        with tempfile.TemporaryDirectory(prefix="planar-eval-multitask-") as results_root:
            options = harness.Options(
                mode="lifecycle-fixture",
                vendor="",
                surface="agent",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            context = harness.prepare_lifecycle_fixture(
                harness.ROOT / "evals" / "orchestrator" / "cases" / "classic-lifecycle-success.json",
                case,
                options,
                "fixture",
            )
            self.assertEqual(len(context.task_ids), 2)
            self.assertEqual(context.task_id, context.task_ids[0])
            run_meta = harness.read_json(context.artifacts / "run.json")
            self.assertEqual(run_meta.get("task_ids"), context.task_ids)
            links = harness.run_json(
                ["planar", "links", "list", f"task:{context.task_ids[1]}", "--json"],
                cwd=context.repo,
                env=context.env,
            )
            self.assertEqual(links["relationship"], "depends-on")
            self.assertEqual(str(links["from_id"]), context.task_ids[1])
            self.assertEqual(str(links["to_id"]), context.task_ids[0])


class RateLimitDetectionTest(unittest.TestCase):
    """Guard the live lane against the `rate_limit_event` false positive.

    The Claude CLI emits this telemetry line on every stream-json run. A
    substring match on `rate_limit` therefore blocked every claude/agent run
    regardless of outcome, silently voiding the lane.
    """

    # Captured verbatim from a real claude/agent live run (2026-07-28). Note it
    # carries `overageStatus: "rejected"` while `status` is "allowed" — only
    # `status` is the limiting signal, so this event must not block.
    ALLOWED_EVENT = (
        '{"type":"rate_limit_event","rate_limit_info":{"status":"allowed",'
        '"resetsAt":1785290400,"rateLimitType":"five_hour",'
        '"overageStatus":"rejected","overageDisabledReason":"org_level_disabled",'
        '"isUsingOverage":false},"uuid":"326c4ce5","session_id":"51d48eff"}'
    )

    def test_allowed_event_does_not_block(self) -> None:
        self.assertIsNone(harness.rate_limit_reason(self.ALLOWED_EVENT))

    def test_allowed_event_alongside_transcript_does_not_block(self) -> None:
        transcript = "\n".join(
            ['{"type":"system","subtype":"init"}', self.ALLOWED_EVENT,
             '{"type":"assistant","message":{"model":"claude-opus-5"}}']
        )
        self.assertIsNone(harness.rate_limit_reason(transcript))

    def test_limiting_status_blocks_and_names_itself(self) -> None:
        event = self.ALLOWED_EVENT.replace('"status":"allowed"', '"status":"rejected"')
        reason = harness.rate_limit_reason(event)
        self.assertIsNotNone(reason)
        assert reason is not None
        self.assertIn("rejected", reason)
        self.assertIn("five_hour", reason)

    def test_unknown_status_fails_closed(self) -> None:
        event = self.ALLOWED_EVENT.replace('"status":"allowed"', '"status":"throttled"')
        self.assertIsNotNone(harness.rate_limit_reason(event))

    def test_textual_limit_still_blocks(self) -> None:
        for text in (
            "api_error_status: 429",
            "You have hit your session limit",
            "rate limit exceeded",
        ):
            with self.subTest(text=text):
                self.assertIsNotNone(harness.rate_limit_reason(text))

    def test_unparseable_event_line_does_not_crash(self) -> None:
        self.assertIsNone(harness.rate_limit_reason('{"type":"rate_limit_event"'))

    def test_clean_transcript_does_not_block(self) -> None:
        self.assertIsNone(harness.rate_limit_reason('{"type":"assistant"}'))


class LiveInvocationGrantTest(unittest.TestCase):
    """The phase3 live lane must grant Bash, or intake cannot run at all."""

    def test_allowed_tools_constant_grants_bash(self) -> None:
        self.assertIn("Bash", harness.CLAUDE_LIVE_ALLOWED_TOOLS.split(","))

    def test_allowed_tools_is_a_single_argv_element(self) -> None:
        """`--allowedTools` is variadic: a two-token form eats the prompt.

        Passing it as `--allowedTools`, `"Bash,..."`, `prompt` makes the CLI
        consume the prompt as another tool name and exit 1 with "Input must be
        provided ... when using --print".
        """
        arg = harness.CLAUDE_LIVE_ALLOWED_TOOLS_ARG
        self.assertTrue(arg.startswith("--allowedTools="))
        self.assertNotIn(" ", arg)


class CliMainExitMappingTest(unittest.TestCase):
    """`cli_main` maps arena fail-closed errors like `EvalFailure` (task 6885).

    Before this fix, `main()` raising `VendorAuthError`/`VendorStagingError`/
    `ArenaIsolationError` was uncaught in the `__main__` block and surfaced
    as a Python traceback instead of the harness's normal exit-coded
    failure path. Assert exit code, stderr message, and NO traceback for
    each of the three arena errors, plus the pre-existing `EvalFailure`
    case as a control.
    """

    def _run_cli_main_with(self, exc: BaseException) -> tuple[int, str]:
        buf = io.StringIO()
        with mock.patch.object(harness, "main", side_effect=exc):
            with contextlib.redirect_stderr(buf):
                code = harness.cli_main([])
        return code, buf.getvalue()

    def test_eval_failure_maps_to_exit_1_no_traceback(self) -> None:
        code, stderr = self._run_cli_main_with(harness.EvalFailure("boom"))
        self.assertEqual(code, 1)
        self.assertIn("FAIL: boom", stderr)
        self.assertNotIn("Traceback", stderr)

    def test_arena_isolation_error_maps_to_exit_1_no_traceback(self) -> None:
        code, stderr = self._run_cli_main_with(
            arena.ArenaIsolationError("PLANAR_DB escaped the arena root")
        )
        self.assertEqual(code, 1)
        self.assertIn("FAIL: PLANAR_DB escaped the arena root", stderr)
        self.assertNotIn("Traceback", stderr)

    def test_vendor_staging_error_maps_to_exit_1_no_traceback(self) -> None:
        code, stderr = self._run_cli_main_with(
            arena.VendorStagingError("missing required commands/ directory")
        )
        self.assertEqual(code, 1)
        self.assertIn("FAIL: missing required commands/ directory", stderr)
        self.assertNotIn("Traceback", stderr)

    def test_vendor_auth_error_maps_to_exit_1_no_traceback_and_no_token(self) -> None:
        code, stderr = self._run_cli_main_with(
            arena.VendorAuthError("CLAUDE_CODE_OAUTH_TOKEN not set")
        )
        self.assertEqual(code, 1)
        self.assertIn("FAIL: CLAUDE_CODE_OAUTH_TOKEN not set", stderr)
        self.assertNotIn("Traceback", stderr)
        # The message names the missing variable, never a credential value.
        self.assertNotIn("sk-", stderr)


class DocsHonestyTest(unittest.TestCase):
    """The eval docs claim only gated targets (task 6834, spec § C6).

    `evals/README.md` and `evals/orchestrator/evidence.md` must not claim an
    unowned cadence (CI, nightly, release, smoke, "every change", "Full
    Stack"). There is no CI configuration in this repository (`.github/`
    does not exist) and `evals/RESULTS.md` does not exist yet (planned for
    a later milestone), so a bare cadence claim is a documentation-honesty
    violation unless the line does one of two things:

    1. DENIES the cadence (e.g. "no CI", "not run in CI", "no automated",
       "operator-invoked only") -- a line saying the term does NOT apply
       is not a claim that it does.
    2. States the term is GATED BY/VIA a specific `make <target>` this
       repo's Makefile actually defines (e.g. "gated by `make
       eval-orchestrator-fast`") -- naming the actual invocation, not
       just mentioning some target elsewhere in the same sentence.

    (task 6878): the prior rule accepted ANY line that merely contained
    both the term and a backtick-quoted `make <target>` ANYWHERE in it,
    with no requirement that the target actually be what gates the term.
    That let a sentence like "`make eval-orchestrator-fast` runs nightly
    in CI" through, because it happens to mention a real target -- while
    still asserting the false, unowned "nightly in CI" cadence. Requiring
    a denial phrase or the specific "gated by/via `make X`" construction
    closes that: a naked cadence claim next to an unrelated target mention
    no longer passes.
    """

    REPO_ROOT = Path(__file__).resolve().parent.parent.parent
    CHECKED_FILES = (
        REPO_ROOT / "evals" / "README.md",
        REPO_ROOT / "evals" / "orchestrator" / "evidence.md",
    )
    TERMS = ("CI", "nightly", "Full Stack", "release", "smoke", "every change")
    RESULTS_LEDGER = "evals/RESULTS.md"

    # A line matching one of these DENIES the cadence rather than claiming
    # it, so a TERM appearing inside one is not a violation.
    DENIAL_PATTERNS = (
        re.compile(r"\bno\s+CI\b", re.IGNORECASE),
        re.compile(r"\bnot\s+run\s+in\s+CI\b", re.IGNORECASE),
        re.compile(r"\bno\s+automated\b", re.IGNORECASE),
        re.compile(r"\boperator-invoked\s+only\b", re.IGNORECASE),
        re.compile(r"\bnever\s+runs?\s+automatically\b", re.IGNORECASE),
    )

    # A line matching this states the term is GATED BY/VIA a specific
    # `make <target>` -- the target named is the thing that produces the
    # cadence, not merely something else mentioned in the same sentence.
    GATE_PATTERN = re.compile(
        r"\bgated\s+(?:by|via)\s+`make\s+([A-Za-z0-9_.-]+)`", re.IGNORECASE
    )

    @classmethod
    def _makefile_targets(cls) -> set[str]:
        makefile_text = (cls.REPO_ROOT / "Makefile").read_text(encoding="utf-8")
        return set(re.findall(r"(?m)^([A-Za-z0-9_.-]+):", makefile_text))

    # Strips backtick-quoted code spans before term matching so a doc line
    # merely NAMING a command or verb (`` `make smoke` ``, `` `planar-agent
    # release` ``) does not trip a cadence-claim check aimed at prose
    # (task 6882): a code span is a literal, not a cadence assertion.
    _CODE_SPAN = re.compile(r"`[^`]*`")

    @classmethod
    def _line_matches_term(cls, line: str, term: str) -> bool:
        prose = cls._CODE_SPAN.sub("", line)
        return re.search(r"\b" + re.escape(term) + r"\b", prose) is not None

    @classmethod
    def _line_is_honest(cls, line: str, targets: set[str]) -> bool:
        if cls.RESULTS_LEDGER in line:
            return True
        if any(pattern.search(line) for pattern in cls.DENIAL_PATTERNS):
            return True
        gate_match = cls.GATE_PATTERN.search(line)
        if gate_match and gate_match.group(1) in targets:
            return True
        return False

    @classmethod
    def _find_violations(cls, path: Path, text: str, targets: set[str]) -> list[str]:
        violations: list[str] = []
        for lineno, line in enumerate(text.splitlines(), start=1):
            for term in cls.TERMS:
                if not cls._line_matches_term(line, term):
                    continue
                if cls._line_is_honest(line, targets):
                    continue
                violations.append(
                    f"{path}:{lineno}: claims {term!r} without a denial "
                    f"phrase or a 'gated by/via `make <target>`' anchor "
                    f"naming a real Makefile target: {line.strip()!r}"
                )
        return violations

    def test_cadence_claims_name_a_gated_target_or_the_results_ledger(self) -> None:
        targets = self._makefile_targets()
        violations: list[str] = []

        for path in self.CHECKED_FILES:
            text = path.read_text(encoding="utf-8")
            violations.extend(
                self._find_violations(path.relative_to(self.REPO_ROOT), text, targets)
            )

        self.assertEqual(
            violations,
            [],
            "cadence claim not anchored to a denial phrase or a gated "
            "target:\n" + "\n".join(violations),
        )

    def test_no_ci_configuration_exists(self) -> None:
        """Guards the premise: if CI ever lands, this test (and the docs) must change."""
        self.assertFalse((self.REPO_ROOT / ".github").exists())

    def test_seeded_counter_example_is_rejected(self) -> None:
        # Task 6878's motivating counter-example: a naked "runs nightly in
        # CI" cadence claim sitting next to an unrelated, but real, `make`
        # target mention used to pass under the old "any target anywhere
        # in the line" rule. Seed it into a MUTATED COPY of the real
        # `evals/README.md` (never the checked-in file itself) and assert
        # the seeded copy is rejected.
        targets = self._makefile_targets()
        real_target = next(iter(targets))
        real_readme = (self.REPO_ROOT / "evals" / "README.md").read_text(
            encoding="utf-8"
        )
        counter_example_line = (
            f"`make {real_target}` runs nightly in CI to keep the suite green.\n"
        )

        with tempfile.TemporaryDirectory(prefix="planar-eval-docs-honesty-") as tmp:
            seeded_path = Path(tmp) / "README.md"
            seeded_path.write_text(real_readme + counter_example_line, encoding="utf-8")
            violations = self._find_violations(
                seeded_path, seeded_path.read_text(encoding="utf-8"), targets
            )
            self.assertTrue(
                violations,
                "seeded counter-example ('runs nightly in CI' beside a "
                "real but unrelated make target) was NOT rejected -- the "
                "honesty check has regressed to the old "
                "any-target-anywhere rule",
            )
            self.assertTrue(
                any("nightly" in v for v in violations),
                f"violation list did not flag the seeded line: {violations}",
            )

            # The ORIGINAL, un-mutated README must stay clean under the
            # same check -- proves the seeded line is what triggers it,
            # not some unrelated pre-existing content.
            self.assertEqual(
                self._find_violations(seeded_path, real_readme, targets), []
            )

        # A legitimately gated cadence claim, using the same real target,
        # must still pass -- proves the fixture isn't failing everything.
        honest_example = f"Cleanup is gated by `make {real_target}`.\n"
        self.assertEqual(
            self._find_violations(Path("honest.md"), honest_example, targets), []
        )

        # A denial is honest even with no target at all.
        denial_example = "There is no CI in this repository.\n"
        self.assertEqual(
            self._find_violations(Path("denial.md"), denial_example, targets), []
        )

    def test_term_inside_backticks_is_not_a_cadence_claim(self) -> None:
        """Naming a command/verb in code span is not a cadence claim (task 6882)."""
        targets = self._makefile_targets()

        code_span_examples = (
            "Run `make smoke` to check the binary locally.\n",
            "The `planar-agent release` verb transitions the claim.\n",
        )
        for example in code_span_examples:
            self.assertEqual(
                self._find_violations(Path("codespan.md"), example, targets),
                [],
                f"code-span occurrence wrongly flagged as a cadence claim: {example!r}",
            )

    def test_bare_prose_term_outside_backticks_still_fails(self) -> None:
        """Word-boundary matching does not blunt genuine bare cadence claims."""
        targets = self._makefile_targets()

        violations = self._find_violations(
            Path("prose.md"), "We release nightly to keep things fresh.\n", targets
        )
        self.assertTrue(
            violations,
            "an unanchored 'release'/'nightly' cadence claim outside "
            "backticks must still be flagged",
        )


class LifecycleFixtureNegativeControlWiringTests(unittest.TestCase):
    """Task 6892: proves `main()`'s `--lifecycle-fixture-only` branch
    actually CALLS `run_lifecycle_fixture_negative_control` (the real
    caller `make eval-orchestrator-fixtures` runs), without spawning the
    real `planar`/`planar-agent`/git processes a full fixture replay
    needs. CALL-SITE test: mutant is dropping the `main()` call to
    `run_lifecycle_fixture_negative_control` -- `control_mock` would then
    never be invoked and `assert_called_once()` fails.
    """

    def test_main_lifecycle_fixture_mode_invokes_the_negative_control(
        self,
    ) -> None:
        with mock.patch.object(
            harness, "run_lifecycle_fixture_negative_control"
        ) as control_mock, mock.patch.object(
            harness, "run_concurrent_coders_negative_controls"
        ) as concurrent_control_mock, mock.patch.object(
            harness, "collect_case_failures", return_value=[]
        ), mock.patch.object(harness, "require_commands"):
            code = harness.main(["--lifecycle-fixture-only"])
        self.assertEqual(code, 0)
        control_mock.assert_called_once()
        # Task 6853 (reviewer re-dispatch): the concurrent-coders negative
        # control is a SEPARATE call from the classic one above -- a
        # mutant that drops only this call while leaving the classic one
        # intact must still fail this test.
        concurrent_control_mock.assert_called_once()
        # The negative control must run against every loaded case, not
        # just whatever `--case` narrowed `selected` to -- a `--case`
        # filter that only selects one case must never skip it.
        cases_arg = control_mock.call_args.args[0]
        self.assertGreaterEqual(len(cases_arg), 1)
        concurrent_cases_arg = concurrent_control_mock.call_args.args[0]
        self.assertGreaterEqual(len(concurrent_cases_arg), 1)

    def test_negative_control_rejects_a_run_that_never_fails(self) -> None:
        # If a case's forbidden_events no longer includes the seeded
        # violation event (or the replay stops failing under the seed),
        # the negative control itself must be a suite failure, not a
        # silent pass -- proven here without a real fixture run by
        # stubbing `run_lifecycle_fixture_replay` to succeed.
        case = {
            "id": "fake-lifecycle-case",
            "tiers": ["lifecycle"],
            "expected": {"forbidden_events": ["task-completed-before-review"]},
        }
        options = harness.Options(
            mode="lifecycle-fixture",
            vendor="",
            surface="skill",
            case_filter=None,
            results_dir=None,
            keep=True,
        )
        with mock.patch.object(harness, "run_lifecycle_fixture_replay"):
            with self.assertRaisesRegex(
                harness.EvalFailure, "negative control was not detected"
            ):
                harness.run_lifecycle_fixture_negative_control(
                    [(Path("<probe>"), case)], options
                )

    def test_negative_control_requires_a_candidate_case(self) -> None:
        options = harness.Options(
            mode="lifecycle-fixture",
            vendor="",
            surface="skill",
            case_filter=None,
            results_dir=None,
            keep=True,
        )
        with self.assertRaisesRegex(
            harness.EvalFailure, "needs a repository case"
        ):
            harness.run_lifecycle_fixture_negative_control([], options)


class ConcurrentCodersNegativeControlWiringTests(unittest.TestCase):
    """Task 6853 (reviewer re-dispatch): the same wiring proofs as
    `LifecycleFixtureNegativeControlWiringTests`, for
    `run_concurrent_coders_negative_controls` -- stubbed
    `run_lifecycle_fixture_replay` so these stay fast unit tests; the
    REAL end-to-end proof (that the driver's call sites actually fire
    under each seed) lives in `make eval-orchestrator-fixtures`, which
    runs `run_concurrent_coders_negative_controls` for real.
    """

    def concurrent_case(self) -> dict[str, object]:
        return {
            "id": "concurrent-coders-fanin",
            "tiers": ["lifecycle"],
            "setup": {"replay": "concurrent-coders"},
        }

    def test_negative_control_requires_a_candidate_case(self) -> None:
        with self.assertRaisesRegex(harness.EvalFailure, "needs a repository case"):
            harness.run_concurrent_coders_negative_controls([], lifecycle_options())

    def test_negative_control_rejects_a_run_that_never_fails(self) -> None:
        # If the driver stops raising under a seed (e.g. a no-op'd guard
        # call site), this must be a suite failure, not a silent pass.
        with mock.patch.object(harness, "run_lifecycle_fixture_replay"):
            with self.assertRaisesRegex(
                harness.EvalFailure, "negative control was not detected"
            ):
                harness.run_concurrent_coders_negative_controls(
                    [(Path("<probe>"), self.concurrent_case())], lifecycle_options()
                )

    def test_negative_control_rejects_a_failure_for_the_wrong_reason(self) -> None:
        # A raise that names an unrelated problem must not be accepted as
        # proof the seeded guard fired.
        with mock.patch.object(
            harness,
            "run_lifecycle_fixture_replay",
            side_effect=harness.EvalFailure("unrelated: fixture tests failed"),
        ):
            with self.assertRaisesRegex(
                harness.EvalFailure, "failed for the wrong reason"
            ):
                harness.run_concurrent_coders_negative_controls(
                    [(Path("<probe>"), self.concurrent_case())], lifecycle_options()
                )

    def test_negative_control_runs_every_seed(self) -> None:
        # CALL-SITE test: a mutant that only seeds one violation (e.g.
        # returns after the first iteration) must fail this test.
        seen_seeds: list[str] = []

        def fake_replay(path, case, options, *, seed_violation=None):
            seen_seeds.append(seed_violation)
            raise harness.EvalFailure(
                {
                    "concurrent-pull-failure": "x: concurrent pull failed for 1 lane(s): boom",
                    "duplicate-task-claim": "x: concurrent pull produced duplicate claimed tasks: ['1', '1']",
                    "partial-fanin": "x: epic branch does not contain lane commit(s) for: lane-3",
                    "complete-before-fanin": "x: complete observed before the fan-in merge for claim token(s): tok",
                }[seed_violation]
            )

        with mock.patch.object(
            harness, "run_lifecycle_fixture_replay", side_effect=fake_replay
        ):
            harness.run_concurrent_coders_negative_controls(
                [(Path("<probe>"), self.concurrent_case())], lifecycle_options()
            )
        self.assertEqual(sorted(seen_seeds), sorted(harness.CONCURRENT_CODERS_SEEDS))


class SchemaValidationTests(unittest.TestCase):
    """Task 6891: `schema.json` was documentary only -- nothing loaded it,
    so a `required` edit there was prose, not enforcement.
    `validate_against_json_schema` is the minimal stdlib subset checker
    now wired into `validate_case`, and this class exercises the
    constructs the real `schema.json` uses (type, required, properties,
    enum, items, const, pattern, minLength, minItems, uniqueItems,
    minimum, maximum, maxProperties, additionalProperties, allOf,
    if/then, contains, not) plus the "unsupported construct is a hard
    error" rule and the real CALL SITE (`load_cases` -> `validate_case`).
    """

    def test_type_mismatch_is_rejected(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "expected type"):
            harness.validate_against_json_schema({"type": "string"}, 5)

    def test_type_match_passes(self) -> None:
        harness.validate_against_json_schema({"type": "string"}, "ok")

    def test_required_names_the_missing_key(self) -> None:
        with self.assertRaisesRegex(
            harness.SchemaValidationError, "missing required key.*widget"
        ):
            harness.validate_against_json_schema(
                {"type": "object", "required": ["widget"]}, {}
            )

    def test_properties_recurses_into_child_schema(self) -> None:
        schema = {
            "type": "object",
            "properties": {"name": {"type": "string", "minLength": 1}},
        }
        with self.assertRaisesRegex(harness.SchemaValidationError, r"\$\.name"):
            harness.validate_against_json_schema(schema, {"name": ""})

    def test_additional_properties_false_rejects_an_extra_key(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "properties": {"name": {"type": "string"}},
        }
        with self.assertRaisesRegex(
            harness.SchemaValidationError, "unexpected additional property: extra"
        ):
            harness.validate_against_json_schema(
                schema, {"name": "ok", "extra": True}
            )

    def test_enum_rejects_a_value_outside_the_set(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "not one of"):
            harness.validate_against_json_schema({"enum": ["a", "b"]}, "c")

    def test_const_rejects_a_mismatched_value(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "expected const"):
            harness.validate_against_json_schema({"const": 1}, 2)

    def test_pattern_rejects_a_non_matching_string(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "does not match"):
            harness.validate_against_json_schema(
                {"pattern": "^[a-z]+$"}, "NOT-LOWER"
            )

    def test_items_recurses_into_every_array_element(self) -> None:
        schema = {"type": "array", "items": {"type": "string"}}
        with self.assertRaisesRegex(harness.SchemaValidationError, r"\$\[1\]"):
            harness.validate_against_json_schema(schema, ["ok", 5])

    def test_min_items_rejects_a_too_short_array(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "minItems"):
            harness.validate_against_json_schema({"minItems": 2}, ["one"])

    def test_unique_items_rejects_a_duplicate(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "not unique"):
            harness.validate_against_json_schema(
                {"uniqueItems": True}, ["a", "a"]
            )

    def test_minimum_and_maximum_bound_a_number(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "below minimum"):
            harness.validate_against_json_schema({"minimum": 30}, 10)
        with self.assertRaisesRegex(harness.SchemaValidationError, "exceeds maximum"):
            harness.validate_against_json_schema({"maximum": 3600}, 9000)

    def test_max_properties_rejects_an_oversized_object(self) -> None:
        with self.assertRaisesRegex(harness.SchemaValidationError, "maxProperties"):
            harness.validate_against_json_schema(
                {"maxProperties": 0}, {"tier": "small"}
            )

    def test_all_of_applies_every_branch(self) -> None:
        schema = {"allOf": [{"type": "string"}, {"minLength": 3}]}
        with self.assertRaisesRegex(harness.SchemaValidationError, "minLength"):
            harness.validate_against_json_schema(schema, "ab")

    def test_if_then_applies_then_only_when_if_matches(self) -> None:
        schema = {
            "if": {"properties": {"tiers": {"contains": {"const": "live"}}}},
            "then": {"required": ["live"]},
        }
        # `if` does not match (no "live" in tiers) -> "then" is skipped.
        harness.validate_against_json_schema(
            schema, {"tiers": ["contract"]}
        )
        # `if` matches -> "then" applies and its violation surfaces.
        with self.assertRaisesRegex(
            harness.SchemaValidationError, "missing required key.*live"
        ):
            harness.validate_against_json_schema(
                schema, {"tiers": ["live"]}
            )

    def test_contains_requires_at_least_one_matching_item(self) -> None:
        schema = {"contains": {"const": "live"}}
        with self.assertRaisesRegex(harness.SchemaValidationError, "contains"):
            harness.validate_against_json_schema(schema, ["contract", "lifecycle"])
        harness.validate_against_json_schema(schema, ["contract", "live"])

    def test_not_rejects_when_the_inner_schema_matches(self) -> None:
        schema = {"not": {"contains": {"const": "live"}}}
        with self.assertRaisesRegex(harness.SchemaValidationError, "must not"):
            harness.validate_against_json_schema(schema, ["live"])
        harness.validate_against_json_schema(schema, ["contract"])

    def test_unsupported_construct_is_a_hard_error_not_a_skip(self) -> None:
        # `$ref` is not in the supported subset. A checker that silently
        # skipped it would pass the (invalid) instance below; the real
        # requirement is a loud failure naming the construct.
        with self.assertRaisesRegex(
            harness.SchemaValidationError, r"unsupported construct.*\$ref"
        ):
            harness.validate_against_json_schema(
                {"type": "object", "$ref": "#/definitions/thing"}, {}
            )

    def test_real_schema_json_validates_every_repository_case(self) -> None:
        # Every case actually committed under evals/orchestrator/cases/
        # must validate clean against the real schema.json -- this would
        # fail loudly if a case and the schema had drifted apart.
        schema = harness.load_case_schema()
        for path in sorted(harness.CASES_DIR.glob("*.json")):
            case = harness.read_json(path)
            harness.validate_against_json_schema(schema, case)

    def test_validate_case_rejects_a_case_missing_a_schema_required_key(
        self,
    ) -> None:
        # Drives validate_case (not validate_against_json_schema directly)
        # so this proves the wiring, and requires the missing key's name
        # to appear in the raised message.
        case = {
            "schema_version": 1,
            "id": "missing-tags-probe",
            "skill": "orchestrator",
            "description": "probe",
            # "tags" deliberately omitted -- schema.json requires it.
            "tiers": ["contract"],
            "contract_assertions": [
                {
                    "id": "probe",
                    "description": "probe",
                    "mode": "any",
                    "paths": ["agents/methodology.md"],
                    "pattern": r"\bthe\b",
                }
            ],
            "expected": {},
        }
        with self.assertRaisesRegex(harness.EvalFailure, "tags"):
            harness.validate_case(Path("<probe>"), case)

    def test_load_cases_call_site_invokes_the_schema_check(self) -> None:
        # CALL-SITE test (task 6891): drives the real caller `load_cases`
        # rather than `validate_case`/`validate_against_json_schema`
        # directly. Mutant: dropping `load_cases` -> `validate_case`'s
        # call to `validate_against_json_schema` (or the wiring inside
        # `validate_case`) would make every real repository case load
        # cleanly despite a schema that now requires a key none of them
        # have, so this test would stop raising and fail.
        mutant_schema = {
            "type": "object",
            "required": ["__task_6891_call_site_marker__"],
        }
        with mock.patch.object(
            harness, "load_case_schema", return_value=mutant_schema
        ):
            with self.assertRaisesRegex(
                harness.EvalFailure, "__task_6891_call_site_marker__"
            ):
                harness.load_cases()


class SelfTestConstructionErrorNamingTests(unittest.TestCase):
    """Task 6893: `SelfTestConstructionError` is raised where only the
    assertion id is in scope (deep inside `seed_selftest_mutation` /
    `run_one_assertion_selftest`), so its own message names the assertion
    but not the case. `run_assertion_selftests` is the real caller that
    also knows the case id, so it is wrapped there to name both.
    """

    def _missing_path_case(self, case_id: str, assertion_id: str) -> dict[str, Any]:
        return {
            "id": case_id,
            "tiers": ["contract"],
            "contract_assertions": [
                {
                    "id": assertion_id,
                    "description": "probe",
                    "mode": "any",
                    "paths": ["agents/does-not-exist-for-task-6893-probe.md"],
                    "pattern": "ZZZ",
                }
            ],
        }

    def test_named_selftest_failure_prefixes_the_case_id_once(self) -> None:
        exc = harness.SelfTestConstructionError("probe-assertion: some detail")
        message = harness.named_selftest_failure("probe-case", exc)
        self.assertEqual(message, "probe-case/probe-assertion: some detail")
        # Idempotent: re-prefixing an already-prefixed message must not
        # double the case id.
        self.assertEqual(
            harness.named_selftest_failure("probe-case", harness.SelfTestConstructionError(message)),
            message,
        )

    def test_run_assertion_selftests_call_site_names_case_and_assertion(
        self,
    ) -> None:
        # CALL-SITE test: drives the real caller `run_assertion_selftests`
        # (what `make eval-orchestrator` runs in contract mode), not the
        # helper directly. Mutant: reverting the wrapping (recording
        # `str(exc)` unprefixed, as before task 6893) drops the case id
        # from the message and this assertion fails.
        case = self._missing_path_case(
            "selftest-naming-probe-case", "selftest-naming-probe-assertion"
        )
        self_tests_run, total_assertions, failures = harness.run_assertion_selftests(
            [(Path("<probe>"), case)]
        )
        self.assertEqual(self_tests_run, 0)
        self.assertEqual(total_assertions, 1)
        self.assertEqual(len(failures), 1)
        label, status, message = failures[0]
        self.assertEqual(status, "fail")
        self.assertEqual(label, "selftest-naming-probe-case/selftest-naming-probe-assertion")
        self.assertIn("selftest-naming-probe-case", message)
        self.assertIn("selftest-naming-probe-assertion", message)


def claude_result_line(cost: float, calls: int = 1) -> str:
    lines = []
    for _ in range(calls):
        lines.append(
            json.dumps(
                {
                    "type": "result",
                    "total_cost_usd": cost,
                    "usage": {"input_tokens": 100, "output_tokens": 50},
                }
            )
        )
    return "\n".join(lines) + "\n"


class UsageExtractionTests(unittest.TestCase):
    def test_claude_usage_sums_cost_and_tokens_across_result_events(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            raw = Path(raw_tmp) / "transcript.jsonl"
            harness.write_text(raw, claude_result_line(0.25))
            usage = harness.extract_usage(raw, "claude")
        self.assertEqual(usage["calls"], 1)
        self.assertAlmostEqual(usage["total_cost_usd"], 0.25)
        self.assertEqual(usage["input_tokens"], 100)
        self.assertEqual(usage["output_tokens"], 50)

    def test_codex_usage_reads_token_count_events(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            raw = Path(raw_tmp) / "transcript.jsonl"
            harness.write_text(
                raw,
                json.dumps(
                    {"type": "token_count", "input_tokens": 40, "output_tokens": 10}
                )
                + "\n",
            )
            usage = harness.extract_usage(raw, "codex")
        self.assertEqual(usage["calls"], 1)
        self.assertIsNone(usage["total_cost_usd"])
        self.assertEqual(usage["input_tokens"], 40)
        self.assertEqual(usage["output_tokens"], 10)

    def test_missing_usage_block_is_a_failure_not_zeros(self) -> None:
        # Task 6859: a transcript that never reports usage is a harness/host
        # defect, not a free run -- must not silently return zeros.
        with tempfile.TemporaryDirectory() as raw_tmp:
            raw = Path(raw_tmp) / "transcript.jsonl"
            harness.write_text(raw, json.dumps({"type": "system"}) + "\n")
            with self.assertRaises(harness.EvalFailure):
                harness.extract_usage(raw, "claude")
            with self.assertRaises(harness.EvalFailure):
                harness.extract_usage(raw, "codex")

    def test_merge_usage_sums_cost_and_tokens_across_turns(self) -> None:
        first = {"calls": 1, "total_cost_usd": 0.1, "input_tokens": 10, "output_tokens": 5}
        second = {"calls": 1, "total_cost_usd": 0.2, "input_tokens": 20, "output_tokens": 8}
        merged = harness.merge_usage(first, second)
        self.assertEqual(merged["calls"], 2)
        self.assertAlmostEqual(merged["total_cost_usd"], 0.3)
        self.assertEqual(merged["input_tokens"], 30)
        self.assertEqual(merged["output_tokens"], 13)

    def test_merge_usage_leaves_cost_none_when_vendor_never_reports_it(self) -> None:
        accumulated = {"calls": 0, "total_cost_usd": None, "input_tokens": 0, "output_tokens": 0}
        turn = {"calls": 1, "total_cost_usd": None, "input_tokens": 5, "output_tokens": 2}
        merged = harness.merge_usage(accumulated, turn)
        self.assertIsNone(merged["total_cost_usd"])


class AllowedToolsCallSiteTests(unittest.TestCase):
    """Proves the PREPARE-time allowlist check (task 6850) is wired into
    the real prepare functions, not just into `vendors.validate_allowed_tools`
    standing alone. A no-op in place of either call site would leave
    `test_vendors.AllowedToolsTests` green while a bad case's typo only
    surfaced later, inside a live transcript.
    """

    def test_prepare_phase3_preview_rejects_an_unknown_tool_before_any_host_call(
        self,
    ) -> None:
        case = {
            "id": "allowlist-guard-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
                "allowed_tools": ["Bash", "NotARealTool"],
            },
        }
        with tempfile.TemporaryDirectory() as results_root:
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            with mock.patch.object(harness, "run_command"), mock.patch.object(
                harness, "run_json"
            ) as run_json_mock, mock.patch.object(
                harness, "logical_sqlite_dump", return_value=""
            ), mock.patch.object(
                harness, "git_state", return_value=""
            ), mock.patch.object(
                arena, "stage_vendor_config"
            ), mock.patch.object(
                arena, "assert_vendor_auth"
            ), mock.patch.object(
                harness, "run_to_file"
            ) as run_to_file_mock:
                run_json_mock.side_effect = [
                    {"id": 1},  # planar init (unused return)
                    {"id": 99},  # plan create
                    {},  # plan next (before)
                ]
                with self.assertRaises(harness.EvalFailure) as ctx:
                    harness.prepare_phase3_preview(
                        harness.CASES_DIR / "allowlist-guard-probe.json", case, options
                    )
                self.assertIn("NotARealTool", str(ctx.exception))
            run_to_file_mock.assert_not_called()


class PrepareRunSplitTests(unittest.TestCase):
    """Task 6849: `--prepare` builds the arena and writes run.json without
    ever invoking a vendor host.
    """

    def test_prepare_phase3_preview_never_calls_run_to_file(self) -> None:
        case = {
            "id": "prepare-only-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
            },
        }
        with tempfile.TemporaryDirectory() as results_root:
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=Path(results_root),
                keep=True,
            )
            with mock.patch.object(harness, "run_command"), mock.patch.object(
                harness, "run_json"
            ) as run_json_mock, mock.patch.object(
                harness, "logical_sqlite_dump", return_value=""
            ), mock.patch.object(
                harness, "git_state", return_value=""
            ), mock.patch.object(
                arena, "stage_vendor_config"
            ), mock.patch.object(
                arena, "assert_vendor_auth"
            ), mock.patch.object(
                harness, "run_to_file"
            ) as run_to_file_mock:
                run_json_mock.side_effect = [
                    {"id": 99},  # plan create
                    {},  # plan next (before)
                ]
                artifact_dir = harness.prepare_phase3_preview(
                    harness.CASES_DIR / "prepare-only-probe.json", case, options
                )
            run_to_file_mock.assert_not_called()
            self.assertTrue((artifact_dir / "run.json").is_file())
            run_meta = harness.read_json(artifact_dir / "run.json")
            self.assertEqual(run_meta["case_id"], "prepare-only-probe")
            self.assertEqual(run_meta["plan_id"], "99")

    def test_main_prepare_flag_never_invokes_a_vendor_host(self) -> None:
        # CALL-SITE test for the `--prepare` CLI wiring in `main()`: a
        # no-op in place of the `prepare_only` branch would fall through to
        # `run_case_trials`/`run_phase3_preview`, which calls `run_to_file`.
        with tempfile.TemporaryDirectory() as results_root, mock.patch.object(
            harness, "prepare_phase3_preview"
        ) as prepare_mock, mock.patch.object(
            harness, "require_commands"
        ), mock.patch.object(
            harness, "require_current_installed_projection"
        ), mock.patch.object(
            harness, "run_to_file"
        ) as run_to_file_mock:
            prepare_mock.return_value = Path(results_root)
            rc = harness.main(
                [
                    "--live",
                    "--prepare",
                    "--vendor",
                    "claude",
                    "--surface",
                    "skill",
                    "--case",
                    "phase3-model-routing-host-boundary",
                ]
            )
        self.assertEqual(rc, 0)
        prepare_mock.assert_called_once()
        run_to_file_mock.assert_not_called()


class SpendCeilingTests(unittest.TestCase):
    """Task 6852: exceeding a case or suite spend ceiling aborts the run,
    reconciles the arena's own claims, and records `status: over-budget`.
    """

    def _prepared_live_artifact_dir(self, tmp_root: Path, case_id: str) -> Path:
        artifact_dir = tmp_root / case_id
        (artifact_dir / "repo").mkdir(parents=True)
        harness.write_json(
            artifact_dir / "run.json",
            {
                "mode": "live",
                "case_id": case_id,
                "case_path": "cases/probe.json",
                "vendor": "claude",
                "surface": "skill",
                "plan_id": "77",
                "repo": "repo",
            },
        )
        return artifact_dir

    def test_case_budget_exceeded_raises_over_budget_and_reconciles(self) -> None:
        case = {
            "id": "budget-guard-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant {{PLAN_ID}}",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
                "budget": {"max_usd": 0.05},
            },
        }
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp_root = Path(raw_tmp)
            artifact_dir = self._prepared_live_artifact_dir(tmp_root, case["id"])
            ledger_path = tmp_root / "RESULTS.md"
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=None,
                keep=True,
            )
            with mock.patch.object(
                arena, "assert_vendor_auth"
            ), mock.patch.object(harness, "run_command") as run_command_mock, \
                mock.patch.object(harness, "run_to_file") as run_to_file_mock, \
                mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):

                def fake_run_to_file(command, output, *, cwd, env, timeout_seconds):
                    harness.write_text(output, claude_result_line(0.25))
                    return 0

                run_to_file_mock.side_effect = fake_run_to_file
                with self.assertRaises(harness.EvalOverBudget):
                    harness.run_phase3_preview_from_prepared(
                        Path("<budget-guard-probe>"), case, options, artifact_dir
                    )
            reconcile_calls = [
                call
                for call in run_command_mock.call_args_list
                if call.args and call.args[0][:2] == ["planar-agent", "reconcile"]
            ]
            self.assertEqual(len(reconcile_calls), 1)
            grade = harness.read_json(artifact_dir / "grade.json")
            self.assertEqual(grade["status"], "over-budget")
            self.assertAlmostEqual(grade["usage"]["total_cost_usd"], 0.25)

    def test_suite_ceiling_accumulates_across_cases_on_one_options_instance(
        self,
    ) -> None:
        case = {
            "id": "suite-budget-guard-probe",
            "tasks": [{"slug": "task-one", "title": "Task one"}],
            "live": {
                "prompt": "irrelevant {{PLAN_ID}}",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
            },
        }
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp_root = Path(raw_tmp)
            ledger_path = tmp_root / "RESULTS.md"
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=None,
                keep=True,
                max_usd=0.3,
            )
            with mock.patch.object(arena, "assert_vendor_auth"), mock.patch.object(
                harness, "run_command"
            ), mock.patch.object(harness, "run_to_file") as run_to_file_mock, \
                mock.patch.object(harness, "collect_live_after"), mock.patch.object(
                    harness, "grade_live_artifacts"
                ), mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):

                def fake_run_to_file(command, output, *, cwd, env, timeout_seconds):
                    harness.write_text(output, claude_result_line(0.2))
                    return 0

                run_to_file_mock.side_effect = fake_run_to_file
                first_dir = self._prepared_live_artifact_dir(tmp_root, "first")
                harness.run_phase3_preview_from_prepared(
                    Path("<first>"), {**case, "id": "first"}, options, first_dir
                )
                second_dir = self._prepared_live_artifact_dir(tmp_root, "second")
                with self.assertRaises(harness.EvalOverBudget):
                    harness.run_phase3_preview_from_prepared(
                        Path("<second>"), {**case, "id": "second"}, options, second_dir
                    )
            self.assertEqual(
                harness.read_json(second_dir / "grade.json")["status"], "over-budget"
            )


class TrialsTests(unittest.TestCase):
    def test_single_trial_calls_entrypoint_directly_with_no_wrapper(self) -> None:
        options = live_options()
        calls = []

        def entrypoint(path, case, opts):
            calls.append(1)

        harness.run_case_trials(entrypoint, Path("<p>"), live_case(), options)
        self.assertEqual(calls, [1])

    def test_over_budget_aborts_the_whole_trials_run_immediately(self) -> None:
        """A ceiling governs the invocation, not one trial. Before this,
        EvalOverBudget (an EvalFailure subclass) was caught per trial, so
        --trials kept spending past the operator's number and a later
        passing trial returned 0 (reviewer, M3 cycle 1)."""
        options = harness.Options(
            mode="live",
            vendor="claude",
            surface="skill",
            case_filter=None,
            results_dir=None,
            keep=True,
            trials=3,
        )
        attempts = {"n": 0}

        def entrypoint(path, case, opts):
            attempts["n"] += 1
            if attempts["n"] == 1:
                raise harness.EvalOverBudget("simulated ceiling")

        with self.assertRaises(harness.EvalOverBudget):
            harness.run_case_trials(entrypoint, Path("<p>"), live_case(), options)
        self.assertEqual(attempts["n"], 1, "remaining trials must not run after a ceiling")

    def test_multiple_trials_report_partial_pass_rate_without_raising(self) -> None:
        options = harness.Options(
            mode="live",
            vendor="claude",
            surface="skill",
            case_filter=None,
            results_dir=None,
            keep=True,
            trials=3,
        )
        attempts = {"n": 0}

        def entrypoint(path, case, opts):
            attempts["n"] += 1
            if attempts["n"] == 2:
                raise harness.EvalFailure("simulated flake")

        harness.run_case_trials(entrypoint, Path("<p>"), live_case(), options)
        self.assertEqual(attempts["n"], 3)

    def test_all_trials_failing_reraises(self) -> None:
        options = harness.Options(
            mode="live",
            vendor="claude",
            surface="skill",
            case_filter=None,
            results_dir=None,
            keep=True,
            trials=2,
        )

        def entrypoint(path, case, opts):
            raise harness.EvalFailure("always fails")

        with self.assertRaises(harness.EvalFailure):
            harness.run_case_trials(entrypoint, Path("<p>"), live_case(), options)

    def test_trials_summary_is_written_when_results_dir_is_set(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            results_dir = Path(raw_tmp)
            options = harness.Options(
                mode="live",
                vendor="claude",
                surface="skill",
                case_filter=None,
                results_dir=results_dir,
                keep=True,
                trials=2,
            )

            def entrypoint(path, case, opts):
                return None

            case = live_case()
            harness.run_case_trials(entrypoint, Path("<p>"), case, options)
            summary = harness.read_json(results_dir / case["id"] / "trials.json")
            self.assertEqual(summary["trials"], 2)
            self.assertEqual(summary["passed"], 2)
            self.assertEqual(summary["pass_rate"], 1.0)

    def test_main_trials_flag_call_site_invokes_run_case_trials(self) -> None:
        # CALL-SITE test: a no-op in place of `run_case_trials` in `main()`'s
        # live branch would fall back to calling `run_phase3_preview` once
        # per case, silently dropping `--trials`.
        with mock.patch.object(harness, "run_case_trials") as trials_mock, \
            mock.patch.object(harness, "require_commands"), mock.patch.object(
                harness, "require_current_installed_projection"
            ):
            rc = harness.main(
                [
                    "--live",
                    "--vendor",
                    "claude",
                    "--surface",
                    "skill",
                    "--case",
                    "phase3-model-routing-host-boundary",
                    "--trials",
                    "5",
                ]
            )
        self.assertEqual(rc, 0)
        trials_mock.assert_called_once()
        self.assertEqual(trials_mock.call_args.args[0], harness.run_phase3_preview)


class ResultsLedgerTests(unittest.TestCase):
    def _case(self, case_id: str = "ledger-probe") -> dict[str, object]:
        return {
            "id": case_id,
            "tiers": ["contract", "live"],
            "live": {
                "host_model": "claude-opus-5",
                "expected_models": {"codex": "gpt-5.6-terra", "claude": "claude-sonnet-5"},
            },
        }

    def test_record_ledger_row_is_append_only(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            artifact_dir = Path(raw_tmp) / "artifacts"
            artifact_dir.mkdir()
            harness.write_json(artifact_dir / "grade.json", {"status": "pass"})
            options = live_options("claude")
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                harness.record_ledger_row(self._case(), artifact_dir, options, "pass")
                first_text = ledger_path.read_text(encoding="utf-8")
                harness.record_ledger_row(self._case(), artifact_dir, options, "pass")
                second_text = ledger_path.read_text(encoding="utf-8")
            self.assertTrue(second_text.startswith(first_text))
            self.assertEqual(second_text.count("| ledger-probe |"), 2)

    def test_ledger_rows_parses_written_rows(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            artifact_dir = Path(raw_tmp) / "artifacts"
            artifact_dir.mkdir()
            harness.write_json(artifact_dir / "grade.json", {"status": "pass"})
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                harness.record_ledger_row(
                    self._case(), artifact_dir, live_options("claude"), "pass"
                )
                rows = harness.ledger_rows()
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["case"], "ledger-probe")
            self.assertEqual(rows[0]["grade"], "pass")

    def test_check_ledger_freshness_flags_case_with_no_row(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                violations = harness.check_ledger_freshness(
                    [(Path("<p>"), self._case())]
                )
            self.assertEqual(len(violations), 1)
            self.assertIn("ledger-probe", violations[0])

    def test_check_ledger_freshness_ignores_a_blocked_only_row(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            artifact_dir = Path(raw_tmp) / "artifacts"
            artifact_dir.mkdir()
            harness.write_json(artifact_dir / "grade.json", {"status": "blocked"})
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                harness.record_ledger_row(
                    self._case(), artifact_dir, live_options("claude"), "blocked"
                )
                violations = harness.check_ledger_freshness(
                    [(Path("<p>"), self._case())]
                )
            self.assertEqual(len(violations), 1)
            self.assertIn("no non-blocked ledger row", violations[0])

    def test_check_ledger_freshness_accepts_a_fresh_pass_row(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            artifact_dir = Path(raw_tmp) / "artifacts"
            artifact_dir.mkdir()
            harness.write_json(artifact_dir / "grade.json", {"status": "pass"})
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                harness.record_ledger_row(
                    self._case(), artifact_dir, live_options("claude"), "pass"
                )
                violations = harness.check_ledger_freshness(
                    [(Path("<p>"), self._case())]
                )
            self.assertEqual(violations, [])

    def test_check_ledger_freshness_flags_a_stale_row(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            ledger_path.write_text(
                harness.LEDGER_PREAMBLE
                + "| 2000-01-01 | ledger-probe | live | claude | skill | "
                "claude-opus-5 | pass | abc123456789 |\n",
                encoding="utf-8",
            )
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                violations = harness.check_ledger_freshness(
                    [(Path("<p>"), self._case())]
                )
            self.assertEqual(len(violations), 1)
            self.assertIn("exceeds ledger.max_age_days", violations[0])

    def test_regrade_artifacts_call_site_records_a_pass_row(self) -> None:
        # CALL-SITE test: proves `regrade_artifacts` (the `--grade-artifacts`
        # entry point) itself writes the ledger row, not just that
        # `record_ledger_row` works standing alone. A no-op in place of this
        # call site would leave every other ledger test green while
        # `--grade-artifacts` never actually populated the ledger.
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            artifact_dir = Path(raw_tmp) / "artifacts"
            artifact_dir.mkdir()
            options = live_options("claude")
            write_live_artifacts(artifact_dir, options)
            harness.write_json(
                artifact_dir / "run.json",
                {
                    "mode": "live",
                    "case_id": live_case()["id"],
                    "vendor": "claude",
                    "surface": "agent",
                },
            )
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                harness.regrade_artifacts(artifact_dir, [(Path("<p>"), live_case())])
                rows = harness.ledger_rows()
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["grade"], "pass")
            self.assertEqual(rows[0]["case"], live_case()["id"])

    def test_regrade_cannot_promote_an_over_budget_run_to_a_pass(self) -> None:
        """The graders only inspect the artifacts that DID land, so a run
        aborted on its spend ceiling grades clean and used to be ledgered
        as `pass` — the ceiling's own record erased by the regrade of it
        (reviewer, M3 cycle 1). The retained verdict wins."""
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            artifact_dir = Path(raw_tmp) / "artifacts"
            artifact_dir.mkdir()
            options = live_options("claude")
            write_live_artifacts(artifact_dir, options)
            harness.write_json(
                artifact_dir / "run.json",
                {
                    "mode": "live",
                    "case_id": live_case()["id"],
                    "vendor": "claude",
                    "surface": "agent",
                },
            )
            # What the abort left behind.
            harness.write_json(
                artifact_dir / "grade.json",
                {"status": "over-budget", "case": live_case()["id"]},
            )
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                with self.assertRaises(harness.EvalFailure):
                    harness.regrade_artifacts(artifact_dir, [(Path("<p>"), live_case())])
                rows = harness.ledger_rows()
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["grade"], "over-budget")

    def test_ledger_check_cli_reports_violation_and_exits_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            ledger_path = Path(raw_tmp) / "RESULTS.md"
            with mock.patch.object(harness, "RESULTS_LEDGER_PATH", ledger_path):
                rc = harness.cli_main(["--ledger-check"])
        self.assertEqual(rc, 1)


class RealLedgerAndCwdIsolationTests(unittest.TestCase):
    """Task 6917: the unit suite must never write to the real, COMMITTED
    `evals/RESULTS.md`, and must never leave a stray directory behind under
    the repo root. Both leaks were real: `SpendCeilingTests` exercises
    `enforce_spend_ceiling`, which calls `record_ledger_row` on an
    over-budget abort without patching `RESULTS_LEDGER_PATH`, and
    `ReplayDriverRegistryTests.test_unregistered_driver_fails_before_any_replay_work`
    drove `live_failure` -> `write_grade` against a placeholder
    `Path("<artifacts>")` that was never a real tmpdir, materializing a
    directory literally named `<artifacts>` in the caller's cwd. Each
    suite run left four duplicate `over-budget` rows in the ledger that had
    to be `git checkout`-reverted before every commit.

    This runs those two classes as a SEPARATE process (`python3 -m
    unittest` from `evals/orchestrator/`, exactly how `make
    eval-orchestrator-unit` runs the whole file) rather than importing and
    calling them in-process, so a fix that only isolates the ledger path
    for tests running inside THIS process would still be caught here.
    """

    def test_suspect_classes_do_not_touch_the_real_ledger_or_leave_stray_dirs(
        self,
    ) -> None:
        ledger_path = harness.RESULTS_LEDGER_PATH
        stray_dir = harness.ROOT / "<artifacts>"
        self.assertTrue(
            ledger_path.is_file(),
            f"precondition failed: real ledger missing at {ledger_path}",
        )
        before_bytes = ledger_path.read_bytes()
        before_mtime_ns = ledger_path.stat().st_mtime_ns
        self.assertFalse(
            stray_dir.exists(),
            "precondition failed: a stray '<artifacts>' directory already "
            "exists under the repo root -- clean it up before re-running "
            "this test so it measures THIS run, not a leftover one",
        )
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "unittest",
                "-v",
                "test_harness.SpendCeilingTests",
                "test_harness.ReplayDriverRegistryTests",
            ],
            cwd=str(Path(__file__).resolve().parent),
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"suspect classes did not pass cleanly:\nstdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}",
        )
        after_bytes = ledger_path.read_bytes()
        after_mtime_ns = ledger_path.stat().st_mtime_ns
        self.assertEqual(
            before_bytes,
            after_bytes,
            "SpendCeilingTests / ReplayDriverRegistryTests modified the "
            f"real, committed results ledger at {ledger_path}",
        )
        self.assertEqual(
            before_mtime_ns,
            after_mtime_ns,
            f"the real results ledger at {ledger_path} was rewritten "
            "(byte-identical content, but the mtime changed)",
        )
        self.assertFalse(
            stray_dir.exists(),
            f"SpendCeilingTests / ReplayDriverRegistryTests left a stray "
            f"directory at {stray_dir}",
        )


if __name__ == "__main__":
    unittest.main()


class ContractAssertionsAreContractTierOnlyTests(unittest.TestCase):
    """`contract_assertions` is required only by the tier that grades them.

    `grade_contract` and `run_assertion_selftests` both skip a case
    without "contract" in `tiers`, so demanding the key from a
    lifecycle-only case produced assertions nothing ever ran -- the first
    two such cases (tasks 6854/6857) reached for grep pins on a doc
    sentence and a C++ source comment, which is the prose-pinning M4
    removes. These cases drive the real caller (`validate_case`) and the
    real on-disk case files, not a helper in isolation.
    """

    def test_a_lifecycle_only_case_may_omit_contract_assertions(self) -> None:
        case = minimal_valid_lifecycle_case("lifecycle-only-no-assertions")
        case["tiers"] = ["lifecycle"]
        case.pop("contract_assertions")
        harness.validate_case(Path("<probe>"), case)

    def test_a_contract_tier_case_still_requires_them(self) -> None:
        case = minimal_valid_lifecycle_case("contract-tier-no-assertions")
        case.pop("contract_assertions")
        with self.assertRaisesRegex(
            harness.EvalFailure, "contract tier requires"
        ):
            harness.validate_case(Path("<probe>"), case)

    def test_a_lifecycle_only_case_that_declares_them_is_still_checked(
        self,
    ) -> None:
        case = minimal_valid_lifecycle_case("lifecycle-only-bad-assertion")
        case["tiers"] = ["lifecycle"]
        case["contract_assertions"] = [
            {"id": "Bad Id", "description": "d", "mode": "all",
             "paths": ["agents/methodology.md"], "pattern": "x"}
        ]
        with self.assertRaisesRegex(
            harness.EvalFailure, "kebab-case|does not match pattern"
        ):
            harness.validate_case(Path("<probe>"), case)

    def test_the_shipped_lifecycle_only_cases_declare_no_assertions(self) -> None:
        # Loads the REAL case files through the real loader: a
        # reintroduced ungraded assertion on either shipped
        # lifecycle-only case fails here.
        for case_path, case in harness.load_cases():
            if case["tiers"] == ["lifecycle"]:
                self.assertNotIn(
                    "contract_assertions",
                    case,
                    f"{case['id']} ({case_path.name}) declares assertions no "
                    "tier grades",
                )
