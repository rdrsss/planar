#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

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


if __name__ == "__main__":
    unittest.main()
