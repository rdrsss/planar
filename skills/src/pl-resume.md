---
description: Resume an in-flight task from zero conversational context, validate readiness first.
origin: docs/cli-reference.md#domain-resume
shared_notes:
    - Active scope and task state come from the CLI; the skill must not read or write workspace context outside it.
    - On return, the session id and vendor are recorded on the snapshot.
    - When the packet's `active_claim.worktree_path` is non-empty, prepend a `cd <path>` directive before running the next-action commands so the resumer operates from the same isolated checkout the prior session used.
slug: pl-resume
vendor:
    claude:
        argument_hint: '[<task-id> | <plan-id>] [--budget <tokens>]'
        invocation_examples: |
            /pl-resume 42
            /pl-resume validate 42
            /pl-resume
---

# Planar Resume ({{.VendorTitle}})

Implements the from-zero resumption contract: a new agent process with no prior conversation history can resume any captured task using a single command.

## What It Does

Produces a structured resume packet containing the task identity, current status, exact next action, plan position, linked Jira/GitHub Issues state, recent session entries, decisions, open questions, linked artifacts, and — when an exclusive claim is held — the worktree path the prior session was running in. Validates whether a task meets resumability criteria before producing the packet, and pulls the operational plane if state is stale.

## CLI Commands

Wraps [`resume`](../../docs/cli-reference.md#domain-resume):

```
planar resume [<task-id> | <plan-id>] [--budget <tokens>] [--no-pull]
planar resume validate <task-id>
```

## When To Invoke

At the start of any new agent session when picking up work from a prior session. Run `resume validate` before `resume` to confirm the task is capture-complete; failures include concrete remediation steps.

## Worktree-Aware Resume

When a prior agent session pulled the task with `planar-agent pull <plan> --worktree <path>` under a worktree-isolated strategy (sequential worktree isolation or `parallel-fanout`, both model-runnable via the `workflows/parallel-dispatch.lua` seam), the active `agent_work_claims` row carries the `worktree_path`. The resume packet surfaces that path in two places:

- `--json`: `active_claim.worktree_path` (string; empty when no path was captured).
- Text: a `worktree:` and `cd:` line under section 8 (Audit Footer).

When the field is non-empty, the resumer MUST prepend a `cd <path>` directive to the resume flow before invoking the next-action commands. This keeps the resumer aligned with the prior session's isolated checkout (epic worktree, cycle worktree, feature branch worktree, etc.) rather than spawning planning commands from the canonical repo root and tripping the worktree-planning gate. When `active_claim` is null or `worktree_path` is empty, no `cd` is required.

For the canonical worktree path/branch/topology conventions (`epic/<plan-slug>` + `cycle/<plan-slug>/<task-slug>`, main checkout stays on master, etc.) see [`docs/concepts.md` §Worktree](../../docs/concepts.md#worktree). The model orchestrator owns that lifecycle through the deterministic seam. For the recovery recipe when the prior coder died and the claim is stale, see [`docs/workflows.md` §Recipe 23](../../docs/workflows.md#recipe-23--recover-a-dead-coder-from-its-worktree).

## Context

Report the resolved scope, task or plan, packet budget, pull policy, vendor,
claim/handoff source, and validate or resume mode.

## Intent

State in one sentence which interrupted work will be validated or reconstructed
from durable state.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` across validation,
optional operational pull, and packet assembly targets. Resume and validation
are reads except for the CLI's explicit pull; name every failed link/system or
packet component. `--no-pull` and absent optional evidence are explained skips.

## Result

Always report `outcome=ok|partial|error`. Return the task ID and status, exact
next action, packet sections present, claim or handoff identity, and verified
worktree/branch when available. A validation-only success applies zero. If an
optional pull fails but a usable packet is produced, report `partial` and the
last verified local state.

## Warnings

Name failed freshness pulls, missing resumability fields, stale claims,
unavailable worktree paths, truncation due to budget, and partial evidence.
Do not warn for an intentionally omitted optional section or a valid legacy
no-worktree path.

## Next actions

Give zero to three executable recommendations, led by `cd <worktree_path>` when
present and then the packet's exact next-action command. Validation failures
lead with their concrete capture or handoff remediation.

## Recovery

For every failed target, give `planar resume validate <task-id>` plus the exact
idempotent retry `planar resume <task-id> [--budget <tokens>] [--no-pull]`.
Name each failed external link's `planar sync status --entity <kind:id>
--system <slug> --json` inspection. A successful pull or packet component is
retained; never imply cross-target rollback.

## Vendor Notes

