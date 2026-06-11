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

Runs read-only CLI commands against the cwd-derived scope and formats the results as a concise tree-shaped summary:

1. `planar scope show` — identifies the cwd-derived scope (one line).
2. Two `planar plan list` calls (one per status) — lists plans currently in flight:
   - `planar plan list --status active`
   - `planar plan list --status paused`
3. Three `planar task list` calls (one per status) — lists open tasks grouped by plan:
   - `planar task list --status todo`
   - `planar task list --status doing`
   - `planar task list --status blocked`
4. `planar question list --status open` — lists unresolved questions.

(`--status` is single-valued; union the results of the separate calls in steps 2 and 3.)

Presents the output as a single summary under approximately 40 lines. Blocked tasks are surfaced first within their plan group.

## What It Does Not Do

- Does not modify state. Read-only.
- Does not query external systems (Jira, GitHub). For external status use `planar sync status`.
- Does not produce a full audit trail. For entity-level audit history use `planar audit trail <entity-ref>`.

## Underlying CLI Verbs

Composes (from [`scope`](../../docs/cli-reference.md#domain-scope), [`plan`](../../docs/cli-reference.md#domain-plan), [`task`](../../docs/cli-reference.md#domain-task), [`question`](../../docs/cli-reference.md#domain-question)):

```
planar scope show
planar plan list --status active
planar plan list --status paused
planar task list --status todo
planar task list --status doing
planar task list --status blocked
planar question list --status open
```

(`--status` is single-valued per call; union results across calls.)

## Output Shape

```
Scope: assoc:project:my-app

Plans in flight
  [<plan-id>] Add billing export (active)
    [<child-plan-id>] CSV serialiser sub-plan (active)

Open tasks
  plan:<plan-id>
    [todo]    <task-id>  Implement CSV serialiser
    [doing]   <task-id>  Wire export endpoint
    [blocked] <task-id>  Add rate-limit headers  <- blocked by: task:<task-id>

Open questions
  [<question-id>]  Which date format for export timestamps?  (task:<task-id>)

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
