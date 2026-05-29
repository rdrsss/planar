---
slug: pl-resume
description: "Resume an in-flight task from zero conversational context, validate readiness first."
source: docs/cli-reference.md#domain-resume
vendor:
  claude:
    argument_hint: "[<task-id> | <plan-id>] [--budget <tokens>]"
    invocation_examples: |
      /pl-resume 42
      /pl-resume validate 42
      /pl-resume
shared_notes:
  - "Active scope and task state come from the CLI; the skill must not read or write workspace context outside it."
  - "On return, the session id and vendor are recorded on the snapshot."
  - "When the packet's `active_claim.worktree_path` is non-empty, prepend a `cd <path>` directive before running the next-action commands so the resumer operates from the same isolated checkout the prior session used."
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

When a prior agent session pulled the task with `planar-agent pull <plan> --worktree <path>` under the [`isolated-sequential`](../../agents/methodology.md#orchestration-strategies) or [`parallel-fanout`](../../agents/methodology.md#orchestration-strategies) strategies, the active `agent_work_claims` row carries the `worktree_path`. The resume packet surfaces that path in two places:

- `--json`: `active_claim.worktree_path` (string; empty when no path was captured).
- Text: a `worktree:` and `cd:` line under section 8 (Audit Footer).

When the field is non-empty, the resumer MUST prepend a `cd <path>` directive to the resume flow before invoking the next-action commands. This keeps the resumer aligned with the prior session's isolated checkout (epic worktree, cycle worktree, feature branch worktree, etc.) rather than spawning planning commands from the canonical repo root and tripping the worktree-planning gate. When `active_claim` is null or `worktree_path` is empty, no `cd` is required.

For the canonical [worktree](../../agents/methodology.md#worktrees) path/branch/topology conventions (`epic/<plan-slug>` + `cycle/<plan-slug>/<task-slug>`, main checkout stays on master, etc.) see the methodology. For the recovery recipe when the prior coder died and the claim is stale, see [`docs/workflows.md` §Recipe 23](../../docs/workflows.md#recipe-23--recover-a-dead-coder-from-its-worktree).

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
