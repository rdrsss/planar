#!/usr/bin/env python3
"""Unit tests for `vendors.py` (task 6849 harness split).

These tests never start a subprocess: `vendors.py` only builds argv lists
and validates static configuration, so every case here is a pure function
call.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import vendors


class ModelArgvTests(unittest.TestCase):
    def test_model_argv_emits_separate_elements(self) -> None:
        self.assertEqual(vendors.model_argv("claude-opus-5"), ["--model", "claude-opus-5"])
        self.assertEqual(vendors.model_argv(None), [])


class AllowedToolsTests(unittest.TestCase):
    def test_default_tools_are_all_known(self) -> None:
        vendors.validate_allowed_tools(vendors.DEFAULT_ALLOWED_TOOLS)

    def test_unknown_tool_raises_naming_the_tool(self) -> None:
        with self.assertRaises(vendors.UnknownToolError) as ctx:
            vendors.validate_allowed_tools(["Bash", "NotARealTool"])
        self.assertEqual(ctx.exception.tool, "NotARealTool")

    def test_workflow_and_agent_are_known(self) -> None:
        # Planar artifact 622: both function under `claude -p --permission-mode
        # dontAsk` with an explicit `--allowedTools` and are exactly the case
        # hh-percase-allowlist exists to unlock.
        vendors.validate_allowed_tools(["Workflow", "Agent"])

    def test_allowed_tools_arg_is_one_element(self) -> None:
        arg = vendors.allowed_tools_arg(["Bash", "Read"])
        self.assertEqual(arg, "--allowedTools=Bash,Read")
        self.assertNotIn(" ", arg)


class PreviewCommandTests(unittest.TestCase):
    def test_codex_skill_surface_uses_orchestrator_shorthand(self) -> None:
        command = vendors.build_preview_command(
            vendor="codex",
            surface="skill",
            plan_id="42",
            prompt="do the thing",
        )
        self.assertEqual(command[0], "codex")
        self.assertEqual(command[-1], "$orchestrator 42\n\ndo the thing")
        self.assertIn("--sandbox", command)

    def test_codex_agent_surface_forces_subagent_delegation(self) -> None:
        command = vendors.build_preview_command(
            vendor="codex",
            surface="agent",
            plan_id="42",
            prompt="do the thing",
        )
        self.assertIn("agent_type=orchestrator", command[-1])
        self.assertIn("do the thing", command[-1])

    def test_claude_skill_surface_uses_slash_command(self) -> None:
        command = vendors.build_preview_command(
            vendor="claude",
            surface="skill",
            plan_id="42",
            prompt="do the thing",
        )
        self.assertEqual(command[0], "claude")
        self.assertNotIn("--agent", command)
        self.assertEqual(command[-1], "/pl-orchestrator 42\n\ndo the thing")
        self.assertIn(vendors.allowed_tools_arg(vendors.DEFAULT_ALLOWED_TOOLS), command)

    def test_claude_agent_surface_passes_agent_flag_and_bare_prompt(self) -> None:
        command = vendors.build_preview_command(
            vendor="claude",
            surface="agent",
            plan_id="42",
            prompt="do the thing",
        )
        self.assertIn("--agent", command)
        self.assertEqual(command[command.index("--agent") + 1], "orchestrator")
        self.assertEqual(command[-1], "do the thing")

    def test_host_model_is_threaded_through_as_separate_argv_elements(self) -> None:
        command = vendors.build_preview_command(
            vendor="claude",
            surface="skill",
            plan_id="42",
            prompt="p",
            host_model="claude-opus-5",
        )
        self.assertIn("--model", command)
        self.assertEqual(command[command.index("--model") + 1], "claude-opus-5")

    def test_custom_allowed_tools_replace_the_default(self) -> None:
        command = vendors.build_preview_command(
            vendor="claude",
            surface="agent",
            plan_id="42",
            prompt="p",
            allowed_tools=("Bash", "Workflow", "Agent"),
        )
        self.assertIn("--allowedTools=Bash,Workflow,Agent", command)


class LifecycleCommandTests(unittest.TestCase):
    def test_codex_initial_turn_embeds_developer_instructions(self) -> None:
        command = vendors.build_lifecycle_command(
            vendor="codex", prompt="go", encoded_instructions='"hi"'
        )
        self.assertIn("developer_instructions=\"hi\"", command)
        self.assertTrue(command[-1].endswith("go"))

    def test_claude_initial_turn_uses_agent_flag(self) -> None:
        command = vendors.build_lifecycle_command(
            vendor="claude", prompt="go", encoded_instructions='"hi"'
        )
        self.assertIn("--agent", command)
        self.assertEqual(command[-1], "go")

    def test_codex_resume_turn_carries_session_id(self) -> None:
        command = vendors.build_lifecycle_resume_command(
            vendor="codex",
            session_id="thread-123",
            response="continue",
            encoded_instructions='"hi"',
        )
        self.assertIn("resume", command)
        self.assertIn("thread-123", command)
        self.assertEqual(command[-1], "continue")

    def test_claude_resume_turn_uses_resume_flag(self) -> None:
        command = vendors.build_lifecycle_resume_command(
            vendor="claude",
            session_id="sess-123",
            response="continue",
            encoded_instructions='"hi"',
        )
        self.assertIn("--resume", command)
        self.assertEqual(command[command.index("--resume") + 1], "sess-123")
        self.assertEqual(command[-1], "continue")


if __name__ == "__main__":
    unittest.main()
