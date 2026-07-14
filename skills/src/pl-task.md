---
slug: pl-task
description: "Create, inspect, update, block, reopen, complete, and cancel tasks within the cwd-derived scope."
source: docs/cli-reference.md#domain-task
cross_scope_writes: true
vendor:
  claude:
    argument_hint: "<add|list|show|update|block|reopen|done|cancel|link|touches> [args]"
    invocation_examples: |
      /pl-task add "Implement payment gateway API" --plan <plan-id> --priority 50
      /pl-task list
      /pl-task done 42
      /pl-task block 43 --on 42
shared_notes:
  - "Resolved scope and task state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Task ({{.VendorTitle}})

Manages tasks — the discrete units of work within a plan or scope.

## What It Does

Creates, lists, updates, blocks, reopens, completes, and cancels tasks. Supports priority ordering, parent/plan membership, blocking and entity relationships, repository touches, and the `next_action` field required by `resume validate`.

## CLI Commands

Wraps [`task`](../../docs/cli-reference.md#domain-task):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar task add <title> [--plan <plan-id>] [--parent <task-id>] [--scope <scope>] [--priority <n>] [--body <text>] [--due <date>] [--next-action <text>]
planar task list [--scope <scope>] [--status <status>] [--plan <plan-id>]
planar task show <task-id> --json
planar task update <task-id> [--title <text>] [--body <text>] [--status <status>] [--priority <n>] [--next-action <text>] [--due <date>] [--plan <plan-id>]
planar task block <task-id> --on <task-id> [--reason <text>]
planar task reopen <task-id> [--status todo|doing|blocked] [--reason <text>]
planar task done <task-id>
planar task cancel <task-id>
planar task link <task-id> <to-kind:to-id> --relationship <kind>
planar task touches add <task-id> <repo-slug> [--path <path>]
planar task touches list <task-id> --json
planar task touches remove <task-id> <repo-slug>
```

## Lifecycle Workflow

1. Resolve the scope from cwd, or require an explicit `--scope <slug>` before a write when cwd is outside the owning project. Use `planar task show <task-id> --json` to confirm an existing target and its stored scope.
2. Apply the requested mutation with the narrowest supported verb: `add`, `update`, `block`, `reopen`, `done`, `cancel`, `link`, or `touches add/remove`. Do not use `--no-scope-check` in routine workflows.
3. After every mutation, read back `planar task show <task-id> --json`. Also read `planar task touches list <task-id> --json` after a touches mutation. Do not infer success from exit code or mutation output alone.
4. Verify and report the task identifier, status, owning plan and parent relationships, blocker or linked/touched relationships affected by the request, and `next_action`. Update `next_action` only when the operator explicitly requested that field, then verify again. Otherwise, if executable work remains and `next_action` is empty or stale, report that condition and recommend the exact command `planar task update <task-id> --next-action <text>` without running it.
5. For a terminal task, report the next executable command for the containing plan, normally `planar plan next <plan-id> --json` or `planar plan closeout <plan-id> --dry-run --json`. For a blocked task, report the blocker and an executable inspection command such as `planar task show <blocker-id> --json`.

`planar task done` is only the manual operator transition for an unclaimed task. Never use `planar task done` followed by `planar-agent release` to finish claimed agent work. A claim owner must keep heartbeating, and the coordinating caller must use exactly one atomic terminal verb: `planar-agent complete`, `fail`, `release`, or `block`.

## Result Contract

Return concise sections using the shared operator-feedback contract:

- **Context:** resolved scope, task target, and requested mode.
- **Intent:** one sentence describing the lifecycle change.
- **Actions:** `attempted`, `applied`, `skipped`, and `failed` counts.
- **Result:** `outcome=ok|partial|error`, verified task id and status, plan/parent/blocker/link/touch relationships, `next_action`, and the next executable command.
- **Warnings:** scope assumptions, guarded or partial failures, and degraded verification.
- **Next actions:** zero to three executable recommendations.
- **Recovery:** the exact idempotent inspect or retry command when needed.

For multiple targets, do not roll back completed independent mutations. List every failed target and its exact retry or inspection command. Omit empty sections except **Result**.

## When To Invoke

When adding concrete work items, changing their lifecycle state, recording dependencies or repository relationships, or selecting the next plan action.

## Context

Report the resolved scope, task target or list filter, parent plan or task,
requested operation, and read or mutation mode.

## Intent

State in one sentence whether the request creates, inspects, updates,
completes, blocks, or links a task.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for task rows and
relationships. Lists and shows have zero applied; an already-matching status or
existing relationship is an expected skip when reported by the CLI.

## Result

Always report `outcome=ok|partial|error`. After add, update, done, block, or
link, read `planar task show <task-id> --json` and return the stable task ID,
status, plan, next action, and verified blocker or relationship. A successful
no-op reports zero applied and why state already matched.

## Warnings

Name missing or ambiguous targets, scope mismatch, invalid transitions,
cycles, unavailable post-state reads, and partial independent results. An empty
list or expected idempotent mutation is not a warning.

## Next actions

Give zero to three executable recommendations based on verified state, such as
`planar task show <task-id> --json`, the next runnable task, or its blocker
inspection command.

## Recovery

For every failure, provide `planar task show <task-id> --json` and the exact
idempotent retry with the original `--scope`, status, next-action, blocker, or
relationship arguments. Never prescribe `planar task done` plus a separate
claim release for coordinated agent work, and never claim independent writes
were rolled back.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
