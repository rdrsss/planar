#!/usr/bin/env python3
from __future__ import annotations

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
        with tempfile.TemporaryDirectory() as raw_tmp:
            root = Path(raw_tmp)
            observed_dir = root / "observed"
            # Same ts, two different wrapper pids -- the lower pid's
            # terminal verb must sort first.
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=3.0,
                    seq=0,
                    pid=500,
                    argv=["complete", "--claim", "tok-2"],
                    stdout=terminal_stdout(claim_token="tok-2"),
                ),
            )
            write_observed_record(
                observed_dir,
                observed_record(
                    ts=3.0,
                    seq=0,
                    pid=100,
                    argv=["complete", "--claim", "tok-1"],
                    stdout=terminal_stdout(claim_token="tok-1"),
                ),
            )
            events_path = root / "events.jsonl"
            events_path.write_text("", encoding="utf-8")
            events = harness.merge_lifecycle_events(observed_dir, events_path)
            # Both name "task-completed"; distinguish by which claim token
            # produced them via the underlying records, read back in the
            # same (ts, pid, seq) order the merge used.
            records = harness.read_observed_records(observed_dir)
            self.assertEqual([r["pid"] for r in records], [100, 500])
            self.assertEqual([e["event"] for e in events], ["task-completed", "task-completed"])


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

    @staticmethod
    def _line_matches_term(line: str, term: str) -> bool:
        if term == "CI":
            return re.search(r"\bCI\b", line) is not None
        return term in line

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


if __name__ == "__main__":
    unittest.main()
