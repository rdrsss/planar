---
slug: pl-task
description: "Add, list, prioritize, block, and complete tasks within the cwd-derived scope."
source: docs/cli-reference.md#domain-task
vendor:
  claude:
    argument_hint: "<add|list|show|done|block|update> [args]"
    invocation_examples: |
      /pl-task add "Implement payment gateway API" --plan 7 --priority 50
      /pl-task list
      /pl-task done 42
      /pl-task block 43 --on 42
shared_notes:
  - "Resolved scope and task state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Task ({{.VendorTitle}})

Manages tasks — the discrete units of work within a plan or scope.

## What It Does

Creates, lists, updates, completes, and blocks tasks. Supports priority ordering, sub-tasks, blocking relationships, and the `next_action` field required by `resume validate`.

## CLI Commands

Wraps [`task`](../../docs/cli-reference.md#domain-task):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. The active scope stack was removed in plan 153 M5; there is no `scope use` to push.

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar task add <title> [--plan <plan-id>] [--priority <n>] [--next-action <text>]
planar task list [--scope <scope>] [--status <status>] [--plan <plan-id>]
planar task show <task-id>
planar task update <task-id> [--status <status>] [--next-action <text>]
planar task done <task-id>
planar task block <task-id> --on <task-id>
planar task link <task-id> <to-kind:to-id> --relationship <kind>
```

## When To Invoke

When adding concrete work items to the database, checking what remains in a session, marking progress, or recording a blocking dependency.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
