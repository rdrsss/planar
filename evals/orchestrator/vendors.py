"""Vendor command construction for the orchestrator eval harness.

`harness.py` used to build the `claude` / `codex` argv four separate times
(live preview, live preview resume-shaped variants, lifecycle initial turn,
lifecycle resume turn) with the flags drifting slightly apart between call
sites. This module is the single place that turns (vendor, surface, prompt,
model, allowed-tools) into an argv list, so every caller shares one
definition of "what does invoking claude/codex actually look like".

Nothing in this module spends model budget: it only builds argv lists and
validates static configuration (tool names). It never itself starts a
subprocess.
"""

from __future__ import annotations

# Default Claude tool allowlist (task 6850 default). `--permission-mode
# dontAsk` suppresses the interactive prompt and denies anything not already
# permitted, so a case that needs a wider surface (e.g. `Workflow`, `Agent` --
# see Planar artifact 622) must declare `allowed_tools` explicitly rather than
# silently inheriting a bigger default.
DEFAULT_ALLOWED_TOOLS: tuple[str, ...] = ("Bash", "Read", "Edit", "Write", "Task")

# The known Claude Code tool surface. This is a STATIC registry, not a live
# query: querying the installed CLI for its real tool list means starting an
# authenticated session (the `system/init` event Planar artifact 622
# observed enumerates it), which spends model budget on every prepare call.
# Case declarations are validated against this static list instead, so
# `hh-percase-allowlist` stays deterministic and free. Keep this list in sync
# with the CLI's documented and artifact-622-observed tool names; a case
# naming a tool absent from both is almost certainly a typo, not a genuine
# unlisted capability.
KNOWN_CLAUDE_TOOLS: frozenset[str] = frozenset(
    {
        "Bash",
        "BashOutput",
        "Read",
        "Edit",
        "Write",
        "Glob",
        "Grep",
        "Task",
        "Workflow",
        "Agent",
        "WebFetch",
        "WebSearch",
        "NotebookEdit",
        "TodoWrite",
        "SlashCommand",
        "KillShell",
        "ExitPlanMode",
    }
)


class UnknownToolError(ValueError):
    """Raised by `validate_allowed_tools` for a tool name not on record."""

    def __init__(self, tool: str):
        self.tool = tool
        super().__init__(f"unknown Claude tool in allowed_tools: {tool}")


def validate_allowed_tools(tools: "list[str] | tuple[str, ...]") -> None:
    """Fail closed on a tool name outside `KNOWN_CLAUDE_TOOLS`.

    Raises `UnknownToolError` naming the first offending tool -- this is the
    task-6850 prepare-time check. Case authors get an error naming the exact
    typo instead of the CLI's own opaque "not permitted" refusal appearing
    much later, mid-run, inside a transcript.
    """
    for tool in tools:
        if tool not in KNOWN_CLAUDE_TOOLS:
            raise UnknownToolError(tool)


def allowed_tools_arg(tools: "list[str] | tuple[str, ...]") -> str:
    """Render a tool list as ONE argv element.

    `--allowedTools` is variadic on the underlying parser, so passing it as
    two argv tokens (`"--allowedTools"`, `"Bash,Read"`) swallows whatever
    positional follows it. The `--flag=value` form keeps it to one element.
    """
    return "--allowedTools=" + ",".join(tools)


def model_argv(host_model: "str | None") -> list[str]:
    """Argv fragment selecting the orchestration host model.

    Returned as separate elements so the value is never re-parsed into shell
    syntax.
    """
    return ["--model", host_model] if host_model else []


def build_preview_command(
    *,
    vendor: str,
    surface: str,
    plan_id: str,
    prompt: str,
    host_model: "str | None" = None,
    allowed_tools: "list[str] | tuple[str, ...]" = DEFAULT_ALLOWED_TOOLS,
) -> list[str]:
    """Build the argv for the live phase3-preview invocation.

    `vendor` is `"codex"` or `"claude"`; `surface` is `"skill"` or `"agent"`.
    """
    if vendor == "codex":
        if surface == "agent":
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
        return [
            "codex",
            "exec",
            *model_argv(host_model),
            "--json",
            "--sandbox",
            "workspace-write",
            adapter,
        ]
    if surface == "agent":
        return [
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
            allowed_tools_arg(allowed_tools),
            prompt,
        ]
    return [
        "claude",
        *model_argv(host_model),
        "-p",
        "--output-format",
        "stream-json",
        "--verbose",
        "--permission-mode",
        "dontAsk",
        allowed_tools_arg(allowed_tools),
        f"/pl-orchestrator {plan_id}\n\n{prompt}",
    ]


def build_lifecycle_command(
    *,
    vendor: str,
    prompt: str,
    encoded_instructions: str,
    allowed_tools: "list[str] | tuple[str, ...]" = DEFAULT_ALLOWED_TOOLS,
) -> list[str]:
    """Build the argv for the first lifecycle-host turn."""
    if vendor == "codex":
        return [
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
    return [
        "claude",
        "--agent",
        "orchestrator",
        "-p",
        "--output-format",
        "stream-json",
        "--verbose",
        "--permission-mode",
        "dontAsk",
        allowed_tools_arg(allowed_tools),
        prompt,
    ]


def build_lifecycle_resume_command(
    *,
    vendor: str,
    session_id: str,
    response: str,
    encoded_instructions: str,
    allowed_tools: "list[str] | tuple[str, ...]" = DEFAULT_ALLOWED_TOOLS,
) -> list[str]:
    """Build the argv for a resumed lifecycle-host turn."""
    if vendor == "codex":
        return [
            "codex",
            "exec",
            "resume",
            "--json",
            "-c",
            f"developer_instructions={encoded_instructions}",
            session_id,
            response,
        ]
    return [
        "claude",
        "--resume",
        session_id,
        "-p",
        "--output-format",
        "stream-json",
        "--verbose",
        "--permission-mode",
        "dontAsk",
        allowed_tools_arg(allowed_tools),
        response,
    ]
