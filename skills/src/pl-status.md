---
slug: pl-status
description: "Summarize the cwd-derived scope's state — active plans, open tasks, open questions, blocked items."
source: docs/cli-reference.md#domain-scope
vendor:
  claude:
    argument_hint: "[]"
    invocation_examples: |
      /pl-status
shared_notes:
  - "Resolved scope and database state come from the CLI; the skill must not read or write workspace context outside it."
---

# Pl-Status ({{.VendorTitle}})

Summarize the cwd-derived scope's state — what's active, what's open, what needs attention.

## When To Invoke

At the start of a session, or whenever you want a quick orientation on what's in flight without manually composing `plan list` / `task list` / `question list`.

## What It Does

Runs four read-only CLI commands against the cwd-derived scope and formats the results as a concise tree-shaped summary:

1. `planar scope show` — identifies the cwd-derived scope (one line).
2. `planar plan list --status active --status paused` — lists plans currently in flight.
3. `planar task list --status todo --status doing --status blocked` — lists open tasks grouped by plan.
4. `planar question list --status open` — lists unresolved questions.

Presents the output as a single summary under approximately 40 lines. Blocked tasks are surfaced first within their plan group.

## What It Does Not Do

- Does not modify state. Read-only.
- Does not query external systems (Jira, GitHub). For external status use `planar sync status`.
- Does not produce a full audit trail. For entity-level audit history use `planar audit trail <entity-ref>`.

## Underlying CLI Verbs

Composes (from [`scope`](../../docs/cli-reference.md#domain-scope), [`plan`](../../docs/cli-reference.md#domain-plan), [`task`](../../docs/cli-reference.md#domain-task), [`question`](../../docs/cli-reference.md#domain-question)):

```
planar scope show
planar plan list --status active --status paused
planar task list --status todo --status doing --status blocked
planar question list --status open
```

## Output Shape

```
Scope: assoc:project:my-app

Plans in flight
  [42] Add billing export (active)
    [45] CSV serialiser sub-plan (active)

Open tasks
  plan:42
    [todo]    37  Implement CSV serialiser
    [doing]   38  Wire export endpoint
    [blocked] 39  Add rate-limit headers  ← blocked by: task:41

Open questions
  [5]  Which date format for export timestamps?  (task:37)

Summary: 2 plans · 3 tasks (1 blocked) · 1 open question
```

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
