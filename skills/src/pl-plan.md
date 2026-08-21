---
description: Create, update, inspect, recommend, advance, and close out plans and their steps.
origin: docs/cli-reference.md#domain-plan
shared_notes:
    - Resolved scope and plan state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-plan
vendor:
    claude:
        argument_hint: <create|show|list|update|step|link|next|recommend-strategy|closeout> [args]
        invocation_examples: |
            /pl-plan create "Implement billing module"
            /pl-plan show <plan-id>
            /pl-plan step add <plan-id> "Design the data model"
            /pl-plan link <plan-id> artifact:<artifact-id> --relationship cites
---

# Planar Plan ({{.VendorTitle}})

Manages plans — the top-level structured intent for a body of work.

## What It Does

Creates and updates plans, decomposes them into ordered steps, tracks step progress, links related entities, recommends an execution strategy, selects next work, and closes out eligible plans.

## CLI Commands

Wraps [`plan`](../../docs/cli-reference.md#domain-plan):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar plan create <title> [--scope <scope>] [--parent <plan-id>] [--summary <text>] [--status <status>]
planar plan show <plan-id> --json
planar plan list [--scope <scope>] [--status <status>]
planar plan update <plan-id> [--title <text>] [--slug <slug>] [--summary <text>] [--status <status>] [--parent <plan-id>]
planar plan step add <plan-id> <body> [--after <ordinal>]
planar plan step done <step-id>
planar plan step skip <step-id>
planar plan step link <step-id> <task-id>
planar plan link <plan-id> <to-kind:to-id> --relationship <kind>
planar plan next <plan-id> [--include-claimed] [--include-stale] --json
planar plan recommend-strategy <plan-id> [--closure-source declared|derived] --json
planar plan recompute-status --plan <plan-id> --json
planar plan closeout <plan-id> [--dry-run] --json
```

## Lifecycle Workflow

1. Resolve the scope from cwd, or require `--scope <slug>` when writing from outside the owning project. For an existing plan, inspect `planar plan show <plan-id> --json` before mutation and confirm its stored scope and relationships.
2. Apply the narrowest supported mutation: `create`, `update`, `step add/done/skip/link`, or `link`. Use `recompute-status --plan` only to repair a plan status that disagrees with its task aggregate; do not substitute it for normal task lifecycle transitions.
3. After every mutation, read back `planar plan show <plan-id> --json`. Verify the plan identifier, status, parent/child and entity relationships, steps and linked tasks affected by the request. Do not infer success from exit code or mutation output alone.
4. Run `planar plan next <plan-id> --json` to identify the next executable work and distinguish ready, claimed, blocked, and stale work. Run `planar plan recommend-strategy <plan-id> --json` when the operator asks how to execute the plan; report the recommendation and its evidence, not a guessed strategy.
5. Before closure, run `planar plan closeout <plan-id> --dry-run --json`. If eligible and closure was requested, run `planar plan closeout <plan-id> --json`, then verify with both `plan show --json` and `plan next --json`. If ineligible, report the returned blockers and their exact inspection commands without forcing a status update.

Task completion under an active agent claim remains outside the operator plan workflow: never replace the atomic claim terminal ritual with `planar task done` plus `planar-agent release`. The coordinating caller must use exactly one of `planar-agent complete`, `fail`, `release`, or `block`.

## Result Contract

Return concise sections using the shared operator-feedback contract:

- **Context:** resolved scope, plan target, and requested mode.
- **Intent:** one sentence describing the lifecycle operation.
- **Actions:** `attempted`, `applied`, `skipped`, and `failed` counts.
- **Result:** `outcome=ok|partial|error`, verified plan id and status, parent/child/entity and step/task relationships, strategy recommendation when requested, and the next executable action or closeout blocker.
- **Warnings:** scope assumptions, guarded or partial failures, and degraded verification.
- **Next actions:** zero to three executable recommendations.
- **Recovery:** the exact idempotent inspect, retry, or dry-run command when needed.

For multiple targets, do not roll back completed independent mutations. List every failed target and its exact retry or inspection command. Omit empty sections except **Result**.

## When To Invoke

When starting or updating structured work, decomposing it, selecting execution strategy or next work, or verifying that a plan is ready to close.

## Context

Report the resolved scope, plan or step target, requested operation, parent or
relationship target, and read or mutation mode.

## Intent

State in one sentence whether the request creates, inspects, updates,
decomposes, advances, or links a plan.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for plans, steps,
and links. Read-only list/show operations have zero applied; already-satisfied
step state or existing links are expected skips when the CLI reports them so.

## Result

Always report `outcome=ok|partial|error`. After every create, update, step, or
link mutation, read `planar plan show <plan-id> --json` and return the stable
plan ID, status, affected step or relationship, and verified post-state. A
successful no-op reports zero applied and its reason.

## Warnings

Name missing or ambiguous targets, scope mismatch, invalid transitions,
unavailable post-state reads, and partial independent mutations. An empty list
or expected idempotent state is not a warning.

## Next actions

Give zero to three executable recommendations tied to the verified state, such
as `planar plan show <plan-id> --json` or the next step command.

## Recovery

For a failed target, provide `planar plan show <plan-id> --json` and the exact
idempotent retry using the original `--scope`, parent, status, step, or
relationship arguments. Never imply that an independently created plan, step,
or link was rolled back.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
