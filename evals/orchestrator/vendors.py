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

import json

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


class ToolSurfaceDriftError(ValueError):
    """Raised when a case declares a tool the live host does not expose.

    Distinct from `UnknownToolError`, which is the prepare-time TYPO check
    against the static registry. This one is the post-run DRIFT check: the
    name was on record and still passed validation, but the host that
    actually ran never offered it, so the allowlist entry was inert and
    whatever the case meant to permit was silently not permitted.
    """

    def __init__(self, missing: "list[str]", live: "frozenset[str]"):
        self.missing = sorted(missing)
        self.live = live
        super().__init__(
            "case declares tool(s) the live host did not expose: "
            + ", ".join(self.missing)
            + "; live surface: "
            + ", ".join(sorted(live))
        )


def extract_live_tool_surface(raw_transcript: str) -> "frozenset[str] | None":
    """Return the tool names the host enumerated in its `system/init` event.

    Claude's stream-json protocol opens every session with
    `{"type": "system", "subtype": "init", ..., "tools": [...]}` (Planar
    artifact 622 observed the shape). That list is the host's REAL tool
    surface for the run, and any live or lifecycle run has already paid for
    it -- reading it back costs nothing extra.

    Returns `None` when the transcript carries no init event (a non-Claude
    vendor, or a transcript shape that predates this). Callers treat `None`
    as "nothing to reconcile", never as "no drift".
    """
    for line in raw_transcript.splitlines():
        line = line.strip()
        if not line or not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue
        if event.get("type") != "system" or event.get("subtype") != "init":
            continue
        tools = event.get("tools")
        if isinstance(tools, list) and all(isinstance(t, str) for t in tools):
            return frozenset(tools)
    return None


def reconcile_tool_surface(
    declared: "list[str] | tuple[str, ...]", live: "frozenset[str] | None"
) -> "frozenset[str]":
    """Reconcile a case's declared allowlist against the live tool surface.

    Raises `ToolSurfaceDriftError` for a declared tool the host never
    exposed -- the failure mode `validate_allowed_tools` CANNOT catch,
    because the name is on the static registry and a typo check only
    rejects names that are not. The motivating case is a RENAME: `Task`
    stays spelled correctly in `KNOWN_CLAUDE_TOOLS` and in every case file
    long after the host has renamed it, so every prepare-time check passes
    while the allowlist entry silently does nothing.

    Returns the names in `KNOWN_CLAUDE_TOOLS` that the live host did not
    expose. Those are reported, never raised on: the registry legitimately
    spans hosts, versions and configurations, so a registry entry missing
    from one session is information, not a defect.
    """
    if live is None:
        return frozenset()
    missing = [tool for tool in declared if tool not in live]
    if missing:
        raise ToolSurfaceDriftError(missing, live)
    return frozenset(KNOWN_CLAUDE_TOOLS - live)


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


ORCHESTRATOR_AGENT = "planar-orchestrator"
PLANNER_AGENT = "planar-planner"


def orchestrate_prompt(plan_id: str, prompt: str) -> str:
    """The by-name dispatch prompt for the orchestrator agent."""
    return f"Use the {ORCHESTRATOR_AGENT} agent to orchestrate plan {plan_id}\n\n{prompt}"


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
                f"{ORCHESTRATOR_AGENT} subagent with agent_type={ORCHESTRATOR_AGENT} and "
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
            adapter = orchestrate_prompt(plan_id, prompt)
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
            ORCHESTRATOR_AGENT,
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
        orchestrate_prompt(plan_id, prompt),
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
        ORCHESTRATOR_AGENT,
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


# Task 6865 (plan 1065 M4): the binary each vendor's RAW spawn uses --
# distinct from the builders above, which all wrap the prompt in
# orchestrator-specific (phase3-preview or lifecycle-turn) framing. A raw
# spawn is "run this model with this prompt and nothing else", the shape
# `evals/planning/harness.py` (single-turn planning drafts/grades, claude
# only) and `evals/candidate-spawn/verify.py` (per-candidate spawn probe,
# every vendor in `agents/models.md` §Candidate Presets) both need. Before
# this, each built its own argv inline, which meant three separate places in
# this repo constructed a headless vendor-CLI invocation instead of one --
# undermining the very point of a single choke point for the
# no-headless-llm-shelling boundary. `copilot` and `gemini` are declared
# even though no orchestrator-framed builder above routes to them: a raw
# caller needing those vendors must not be pushed back to inline argv either.
RAW_SPAWN_BINARY: dict[str, str] = {
    "claude": "claude",
    "codex": "codex",
    "copilot": "copilot",
    "gemini": "gemini",
}


def build_raw_command(
    *,
    vendor: str,
    prompt: str,
    host_model: "str | None" = None,
) -> list[str]:
    """Build a minimal, single-turn argv for `vendor`: the vendor's own
    non-interactive/permission defaults, an optional model override, and
    the raw `prompt` as the final positional argument -- no orchestrator
    phase3-preview or lifecycle-turn framing.

    Raises `ValueError` for a vendor this adapter has no raw-spawn shape
    for at all. That is distinct from a vendor whose CLI is simply not
    installed on this machine -- callers check that separately (e.g. with
    `shutil.which(RAW_SPAWN_BINARY[vendor])`) so an absent CLI reports as a
    skip, not an adapter gap.
    """
    if vendor not in RAW_SPAWN_BINARY:
        raise ValueError(f"build_raw_command: unknown vendor {vendor!r}")
    binary = RAW_SPAWN_BINARY[vendor]
    if vendor == "codex":
        return [binary, "exec", *model_argv(host_model), prompt]
    if vendor in ("copilot", "gemini"):
        argv = [binary, "-p", prompt]
        if host_model:
            argv += ["--model", host_model]
        return argv
    # claude
    argv = [binary]
    argv += model_argv(host_model)
    argv += ["-p", "--permission-mode", "dontAsk", prompt]
    return argv
