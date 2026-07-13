---
slug: pl-status
description: "Summarize the cwd-derived scope's state — attention items, active work, claims, handoffs, conflicts, and claim-aware next work."
source: docs/cli-reference.md#domain-dashboard
vendor:
  claude:
    argument_hint: "[]"
    invocation_examples: |
      /pl-status
shared_notes:
  - "Resolved scope and database state come from the CLI; the skill must not read or write workspace context outside it."
---

# Pl-Status ({{.VendorTitle}})

Answer “what needs attention now?” for the cwd-derived scope. This is a
read-only orientation workflow: use the CLI as the only state access layer and
keep the default response to roughly 60 lines or fewer.

## When To Invoke

At session start, before choosing work, or whenever plans, agents, handoffs,
and external sync state need to be reconciled into one concise view.

## Workflow

1. Resolve the scope with `planar scope show --json`. If resolution fails,
   return the feedback contract with `outcome=error` and the exact retry
   command; do not fall back to direct database or workspace inspection.
2. Read the scope roll-up with `planar dashboard --agents --json`. This is the
   canonical source for in-flight plans, active and stale claims, each claim's
   latest action, and next available work by plan.
3. Read blocked tasks with `planar task list --status blocked --json` and
   unresolved questions with `planar question list --status open --json`.
4. Read pending and validated handoffs with two calls because `--status` is
   single-valued here:
   `planar handoff list --status pending --json` and
   `planar handoff list --status validated --json`. Handoffs are global, so
   retain only rows whose task belongs to the resolved scope. Classify a row as
   stale when it is older than 24 hours; show its id, task, status, and age.
   When scope ownership is not present in the list row, verify it with
   `planar task show <task-id> --json` before including the handoff.
5. Read scoped external state with `planar sync status --json` and retain links
   whose status is `conflict` or `error`. A conflict's executable inspection is
   `planar-watch sync-events --entity <kind:id> --outcome conflict --json`;
   resolution remains an explicit operator choice via
   `planar sync resolve <event-id> --keep <local|remote>`.
6. For every in-flight plan, use `planar plan next <plan-id> --json` when the
   dashboard roll-up is insufficient to explain selection. Never choose from
   `task list --status todo` alone: active claims are unavailable, stale claims
   require reconciliation, and blockers remain blocked.

Use JSON internally for stable parsing, then render concise human output.
Command failures are counted and surfaced; never infer success from exit code
alone when a returned object or collection can be checked.

## Attention Order

Render only non-empty groups, in this order:

1. Sync conflicts and errors.
2. Stale claims and stale handoffs.
3. Blocked tasks and open questions.
4. Active claims/actions (claim token prefix, task or plan, vendor/role, lease,
   and latest action summary).
5. Claim-aware next work, ordered by plan priority and then task priority.

Do not print empty headings, empty trees, `none` placeholders, completed work,
or unrelated global handoffs. If the scope is quiet, return only its identity,
a one-line result, and useful next actions. If populated output would exceed
roughly 60 lines, preserve the attention order, show counts plus the highest
priority rows, and name the exact drill-down command for omitted rows.

## Output Contract

Use this shared operator-feedback envelope. Omit empty optional sections, but
always include `Result`.

## Context

Resolved scope, cwd-derived mode, and read-only target.

## Intent

One sentence: summarize what needs attention now and identify safe next work.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed`. For this read-only
workflow `applied` is the number of successful reads and no writes are implied.

## Result

Set `outcome=ok|partial|error`, then render the adaptive attention groups and a
compact count summary. A partial result identifies each failed read.

## Warnings

Report partial failures, stale coordination state, global handoff filtering,
assumptions, and truncated rows. Do not repeat attention items merely to fill
this section.

## Next actions

Give zero to three executable commands selected from the actual highest-priority
items. Prefer, as applicable:

- `planar sync resolve <event-id> --keep <local|remote>` after inspection.
- `planar-agent reconcile --dry-run --plan <plan-id> --json` for stale claims.
- `planar resume validate <task-id> --json` for stale handoffs.
- `planar question show <question-id> --json` for an open decision point.
- `planar plan next <plan-id> --json` for available claim-aware work.

Never recommend claiming a task that the claim-aware result marks claimed,
stale, or blocked.

## Recovery

On partial or error outcomes, provide the exact idempotent failed read to retry.
For a fully successful read, omit this section.

## What It Does Not Do

- Does not mutate tasks, claims, handoffs, questions, or sync state.
- Does not contact Jira or GitHub; `sync status` reads recorded local state.
- Does not replace the full activity feed or audit trail. Use `planar-watch`
  and `planar audit trail <entity-ref>` for those views.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
