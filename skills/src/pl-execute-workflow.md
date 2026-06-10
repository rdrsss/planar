---
slug: pl-execute-workflow
description: "Plan, author, and validate planar-execute Lua workflows with explicit worker model routing intent and context capsules for headless agents."
source: agents/workflow-planner.md
model_tier: large
vendor:
  claude:
    argument_hint: "<plan-id> [--write <path>] [--canonical]"
    invocation_examples: |
      /pl-execute-workflow <plan-id> --write ~/.planar/local/workflows/quality-spine.lua
      /pl-execute-workflow <plan-id> --canonical
shared_notes:
  - "Author workflow sources only where the operator requested. Canonical reusable templates belong under workflows/; operator-specific copies belong outside the repo."
  - "Live execution requires explicit operator opt-in via PLANAR_EXECUTE_LIVE_AGENT=1."
---

# Execute Workflow ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `workflow-planner` agent. See [`agents/workflow-planner.md`](../../agents/workflow-planner.md) for the execution-manifest contract, model-routing policy, and context-capsule rules.

Use this skill to design, write, and validate a Lua workflow for `planar-execute`. The skill owns the implementation step after the workflow planner has produced, or can produce, an execution manifest.

## When to use

- The user asks to write or adapt a `planar-execute` workflow.
- A plan should run through `workflows/quality-spine.lua`, `workflows/parallel-fanout.lua`, or a custom Lua control flow.
- Headless workers need explicit per-task context capsules.
- The operator wants model-tier intent recorded before launching Claude or Codex-style headless sessions.
- A workflow must be validated with `--dry-run`, `--mock-worker`, or `--mock-outcomes` before live execution.

Do not use this skill for normal in-pwd `classic` orchestration; use `pl-orchestrator` for that path.

## Current host constraints

`planar-execute` is the Lua harness. Its documented `ctx.agent(brief, opts)` shape accepts `role`, `worktree_path`, `task_id`, `task_slug`, `claim_token`, and `role_spec`. The live host path selects Claude models **by role** from `src/cmd/planar-execute/role_model.zig` (default: sonnet coder, opus reviewer); it does not expose per-call `vendor`, `tier`, or `model` options in Lua. Per-role models are operator-overridable in `${PLANAR_HOME:-~/.planar}/execute-config.toml` under a `[models]` table — but that is config, not a per-`agent()` argument. Run `planar-execute run --dry-run <workflow.lua>` to print the effective role→model table for the run.

Therefore:

- Record `vendor`, `tier`, and `model_intent` in the execution manifest.
- Put model intent in comments or brief text when useful for audit.
- To actually change which model a role spawns, edit `execute-config.toml` `[models]` (operator-machine config) — do not invent a per-call Lua override.
- Do not emit unsupported Lua options such as `model = "..."` unless the binary has been extended and the CLI reference confirms it.
- If the operator requires actual Codex worker spawning from Lua today, stop and identify that as a host API gap rather than pretending the workflow can enforce it.

## Workflow

1. Read the relevant references:
   - [`docs/cli-reference.md` § Binary: `planar-execute`](../../docs/cli-reference.md#binary-planar-execute)
   - [`docs/workflows.md` Recipe 24](../../docs/workflows.md#recipe-24--author-and-run-a-planar-execute-workflow)
   - [`workflows/README.md`](../../workflows/README.md)
   - [`agents/workflow-planner.md`](../../agents/workflow-planner.md)

2. Build or accept an execution manifest:
   - If the user gave a manifest, validate it against the required fields in `agents/workflow-planner.md`.
   - If not, inspect plan/task state with read-only `planar` and `planar-agent` commands and draft the manifest first.
   - Surface any host gap, missing claim/worktree source, or unsafe live-run request before writing Lua.

3. Choose the output location:
   - Canonical reusable workflows: `workflows/<name>.lua`.
   - Operator-specific workflows: outside the repo, usually under `~/.planar/local/workflows/`.
   - Do not put operator-local workflows into generated vendor dirs.

4. Author Lua from a template:
   - Start with `workflows/quality-spine.lua` for coder -> reviewer cadence.
   - Start with `workflows/parallel-fanout.lua` for N-way parallel dispatch.
   - Use custom Lua only when the manifest needs `ctx.pipeline`, retry loops, or branching the templates do not express.

5. Keep Lua within the host contract:
   - Return `{ meta = ..., run = function(ctx) ... end }`.
   - Use `ctx.agent`, `ctx.parallel`, `ctx.pipeline`, `ctx.eligible`, `ctx.phase`, `ctx.log`, `ctx.workflow`, `ctx.args`, `ctx.now`, and `ctx.seed`.
   - Do not use `os`, `io`, `os.time`, `os.exit`, or `math.random`; the sandbox strips them.
   - Keep `meta.reviewer = true` when the workflow dispatches reviewer cadence or may hit the bright-line guard.

6. Compose briefs from context capsules:
   - The brief must cite paths and entity ids, not paste full specs.
   - Include task id, slug, claim token, worktree path, locked decisions, gates, and report shape.
   - Reviewer briefs must be blind-read briefs. Do not paste coder or test-coder narrative reports.
   - If model intent cannot be enforced through Lua options, include it as manifest metadata and, where useful, an audit line in the brief.

7. Validate before live execution:

   ```sh
   planar-execute run --dry-run <workflow.lua>
   planar-execute run --mock-worker --plan <plan-id> <workflow.lua> <plan-id>
   ```

   For branch/error handling, write an `outcomes.ndjson` file and run:

   ```sh
   planar-execute run --mock-outcomes <outcomes.ndjson> --plan <plan-id> <workflow.lua> <plan-id>
   ```

8. Do not run live unless the user explicitly asks for it. When they do, show the exact command:

   ```sh
   PLANAR_EXECUTE_LIVE_AGENT=1 \
   PLANAR_EXECUTE_STALL_SECS=<n> \
   PLANAR_EXECUTE_MAX_ATTEMPTS=<n> \
   PLANAR_EXECUTE_MAX_TOTAL_SPAWNS=<n> \
   PLANAR_EXECUTE_MAX_WALL_CLOCK_SECS=<n> \
   planar-execute run --plan <plan-id> <workflow.lua> <plan-id>
   ```

## Execution manifest

Write the manifest next to the workflow when the operator asks for a durable artifact. Use `<workflow>.manifest.json` unless the user provides a path. The manifest is audit data for why the workflow dispatches those workers with those context capsules.

Minimum fields:

```json
{
  "schema_version": 1,
  "plan_id": 0,
  "workflow": {
    "name": "",
    "base": "quality-spine",
    "strategy": "isolated-sequential",
    "reviewer_required": true,
    "canonical_candidate": false
  },
  "safety": {
    "meta_reviewer": true,
    "bypass_reviewer_guard_allowed": false,
    "live_allowed": false
  },
  "budgets": {
    "stall_secs": 300,
    "max_attempts": 2,
    "max_total_spawns": 12,
    "max_wall_clock_secs": 7200
  },
  "workers": [],
  "mock_cases": [],
  "validation": {
    "dry_run": "",
    "mock_worker": "",
    "mock_outcomes": []
  }
}
```

## Output report

Return:

- Workflow path written.
- Manifest path written, if any.
- Template used and why.
- Model-routing intent summary and any host API gaps.
- Validation commands run and their outcomes.
- Live command only when explicitly requested or when reporting the operator-ready command without executing it.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
