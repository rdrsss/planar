---
slug: pl-doctor
description: "Guided reconciliation flow for a degraded Planar DB: diagnose contributors, clear stale claims and handoffs, triage non-resumable in-flight tasks."
source: docs/cli-reference.md#domain-health
cross_scope_writes: true
vendor:
  claude:
    argument_hint: "[--json]"
    invocation_examples: |
      /pl-doctor
      /pl-doctor --json
shared_notes:
  - "All writes are operator-confirmed before executing. Do not auto-cancel or auto-abandon without explicit operator approval at each step."
  - "Active scope and database state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Doctor ({{.VendorTitle}})

Guided diagnose-then-reconcile flow for a degraded Planar installation. Companion to `pl-health` (which reports; this one acts). Works entirely through `planar` and `planar-agent` CLI verbs — no direct database writes.

## Safety Contract

Every destructive write (task cancel, task reset, handoff abandon, claim reconcile) requires **explicit operator confirmation** before executing. Recent in-flight tasks (< 48h) are never auto-touched. Reconcile only affects expired claims — never live, heartbeating ones. No `--no-scope-check` is used.

## Scope Warning: Global Health vs Scoped Lists

**`planar health` is global** — it counts in-flight tasks and stale handoffs across the entire database, regardless of current cwd. **`planar task list` is scope-filtered** to the cwd-derived scope; **`planar handoff list` and `planar health` are global** (whole DB, no scope filter). This means a `health --json` report showing `not_resumable_tasks: 3` may not be fully explained by a `task list --status doing,blocked` run from one project's directory. There is no single `task list` flag that spans all scopes: `--scope global` resolves to `scope_kind='global'` and returns only global-scoped tasks — association- and repo-scoped in-flight tasks are silently excluded. To enumerate in-flight tasks across all scopes, run `task list --status doing,blocked` from each project's directory (or pass `--scope <slug>` for each association slug; find slugs via `planar assoc list`). For a quick cross-scope count, `planar health` is the source of truth.

## Step 1 — Diagnose

```
planar health --json
```

Parse the response:

- `overall == "ok"` → installation is healthy; stop and report.
- `overall == "degraded"` → walk the contributors below.

Key fields to inspect:

| Field | Meaning |
|---|---|
| `inflight_tasks` | Tasks in `doing` or `blocked`. |
| `not_resumable_tasks` | In-flight tasks with no `next_action` or no context snapshot. |
| `stale_handoffs` | Handoffs in `pending`/`validated` older than 24 h. |
| `pending_handoffs` | Total handoffs still open (includes fresh ones). |
| `integrity_ok` | SQLite integrity check result. |

If `integrity_ok` is false: stop immediately and escalate. Do not attempt reconciliation on a corrupt database.

## Step 2 — Stale Claims

Preview expired claims without writing:

```
planar-agent reconcile --dry-run --json
```

The JSON includes a `candidates` array — each entry is a claim token that has exceeded its lease. If candidates are present and the operator approves:

```
planar-agent reconcile --json
```

This marks expired claims `stale` and closes orphaned action rows. It is safe: reconcile only touches claims whose lease has already expired. Live, heartbeating claims are never touched.

## Step 3 — Stale Handoffs

List open handoffs across all scopes:

```
planar handoff list --status pending
planar handoff list --status validated
```

For each handoff that is older than 24 h (the `stale_handoffs` count in health), inspect it:

```
planar handoff show <handoff-id>
```

Then, **with explicit operator confirmation**, abandon it:

```
planar handoff abandon <handoff-id> --reason "stale: no resumer after 24h"
```

`handoff abandon` is a terminal transition. A consumed handoff cannot be abandoned — the verb will refuse with a non-zero exit. Inspect `handoff show` first when uncertain of the handoff's current status.

## Step 4 — Non-Resumable In-Flight Tasks

Non-resumable tasks are in `doing` or `blocked` status but lack a `next_action` value, a context snapshot, or both. They block `resume validate` and keep `health` degraded until resolved.

**Never auto-resolve these.** Each one requires operator judgment.

List in-flight tasks for the current project's scope:

```
planar task list --status doing,blocked --json
```

Because `task list` is scope-filtered, this returns only tasks in the cwd-derived scope. Repeat from each project's directory (or pass `--scope <slug>`) for the other associations. Note: `--scope global` returns only global-scoped tasks — it does **not** enumerate all scopes.

For each non-resumable task, gather context before deciding:

```
planar task show <task-id> --json    # see age, plan, scope
planar plan show <plan-id> --json    # see plan status
```

**Classify and triage:**

| Situation | Recommendation |
|---|---|
| Task's plan is `abandoned` or `done` | Cancel the task — it is dead work. Confirm, then: `planar task cancel <task-id>` |
| Task's plan is active; task is weeks stale | Reset to `todo` to preserve the work item. Confirm, then: `planar task update <task-id> --status todo` |
| Task age < 48 h | **Leave it.** This is likely live, legitimate work. Do not touch. |

**Cross-scope writes (important):** `task update` (including `--status` transitions) is scope-guarded: a task in an association other than the current cwd will be rejected with an error like "operator write scope is 'project:my-app'". Pass the task's own association slug via `--scope` to override for that one write. `task cancel`, by contrast, is an id-based status transition with no scope guard — it succeeds from any cwd without `--scope`.

```
# Find the task's scope slug from `task show --json` → look at scope_kind + scope_id,
# then resolve the slug via `planar scope show` or `planar assoc list`.
planar task update <task-id> --scope <task-assoc-slug> --status todo
planar task cancel <task-id>   # no --scope needed; works by id from any cwd
```

**`task update` does not accept `--editor`** — it applies field changes directly from flags. Only `task add` opens an editor. Pass `--status`, `--next-action`, etc. as flags.

Always confirm with the operator before each write.

## Step 5 — Re-Check

Re-run health and report the new state:

```
planar health --json
```

A genuinely-blocked or genuinely in-progress recent task will keep `overall` as `degraded` **by design**. The skill's goal is to clear cruft (expired claims, stale handoffs, zombie tasks in dead plans), not to force green. If the remaining contributors are active work, report that honestly and stop.

## CLI Commands

```
planar health [--json]
planar-agent reconcile [--dry-run] [--stale-after <duration>] [--json]
planar handoff list [--status <status>]
planar handoff show <handoff-id> [--json]
planar handoff abandon <handoff-id> [--reason <text>] [--json]
planar task list [--scope <scope>] [--status <status>] [--json]
planar task show <task-id> [--json]
planar task update <task-id> [--status <status>] [--scope <slug>]
planar task cancel <task-id> [--scope <slug>]
planar plan show <plan-id> [--json]
```

## When To Invoke

When `planar health` exits 1 (degraded) and the operator wants to clear the causes rather than just read the report. Also useful after an unclean shutdown, a died agent session, or a long idle period that left handoffs pending.

## Context

Report the global health mode, current cwd-derived scope, inspected association
slugs, freshness thresholds, and whether the run is diagnosis-only or contains
operator-approved reconciliation.

## Intent

State in one sentence which degraded contributors will be diagnosed and which,
if any, the operator approved for reconciliation.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for expired
claims, stale handoffs, and non-resumable tasks. Count recent or deliberately
retained work as skipped, and name every failed target. Never count a preview
as applied.

## Result

Always report `outcome=ok|partial|error` and the final `planar health --json`
post-state. After a write, also verify the affected handoff with `planar
handoff show <handoff-id> --json`, task with `planar task show <task-id>
--json`, or claim set with `planar-agent reconcile --dry-run --json`. Preserve
the diagnosis and stronger per-contributor safety classification.

## Warnings

Name integrity failures, scopes that could not be enumerated, recent live work,
unavailable post-state reads, and approved actions that failed. A healthy run
or expected decision to retain active work is an informative no-op, not a
warning.

## Next actions

Give zero to three executable recommendations, ordered by the remaining health
contributors. A healthy or fully explained state needs no generic cleanup
advice.

## Recovery

For each failed target, give the exact inspection and safe retry command, such
as `planar handoff show <handoff-id> --json`, `planar task show <task-id>
--json`, or `planar-agent reconcile --dry-run --json`. Require a fresh operator
confirmation before retrying a destructive write; never invent rollback for
independent reconciliations.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
