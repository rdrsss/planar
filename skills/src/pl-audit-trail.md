---
description: For a given external link, show every local session, decision, and commit tied to it.
origin: docs/cli-reference.md#domain-audit
shared_notes:
    - Active scope and audit state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-audit-trail
vendor:
    claude:
        argument_hint: --link <link-id>
        invocation_examples: |
            /pl-audit-trail --link 7
            /pl-audit-trail --json --link 7
---

# Planar Audit Trail ({{.VendorTitle}})

Produces the cross-plane audit trail for an external link — every local session, decision, and attributed commit that produced changes to an external ticket.

## What It Does

Starting from an `external_links` link id, reconstructs the full history of local work tied to that external ticket:

- sessions that touched it
- decisions recorded against the linked entity
- commits attributed to those sessions and now folded into `audit trail`
- sync events with timestamps and vendor identity

Use the trail when you want the whole story in one place. Use `audit commits` when you need to query the commit leg directly by session or task.

## CLI Commands

Wraps [`audit trail`](../../docs/cli-reference.md#domain-audit):

```
planar audit trail --link <link-id>
planar audit session <session-id>
planar audit commits
planar audit commits --session <session-id>
planar audit commits --task <task-id>
```

`audit trail` on a link now includes a commits section when the linked sessions have attributed commits. `audit commits` is the focused query surface for filtering or machine-readable commit output (`--json`, `--shas`); with no filters it lists every recorded commit newest-first.

## When To Invoke

When an auditor or team member needs to understand what agent work produced a given external ticket state, or when debugging a conflict or unexpected field change on an operational plane ticket.

Use `audit trail` for the combined narrative. Use `audit commits` when you need only the attributed commits for a specific session or task, when you need the global commit ledger, or when you need bare SHAs for scripting.

## Context

Report the resolved scope, link, session, or task filter, output mode, and that
the operation is read-only.

## Intent

State in one sentence which external link or local audit leg will be inspected.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` counts for links,
sessions, decisions, commits, and sync-event targets. Audit reads always apply
zero changes; an absent optional commits leg is an expected skip, not a write
failure. Name every target whose evidence could not be read.

## Result

Always report `outcome=ok|partial|error`, zero applied, and the inspected link,
session, task, commit SHAs, and external-system identities returned by the CLI.
An empty trail is an informative `outcome=ok` no-op that says no attributable
records exist for the filter.

## Warnings

Name missing links, unavailable session or commit evidence, and partial reads.
Do not warn merely because an optional audit leg is empty.

## Next actions

Give zero to three executable follow-up reads, such as `planar audit commits
--session <session-id> --json` or `planar sync status --entity <kind:id> --json`.

## Recovery

For each failed read, give the exact target-specific retry, such as `planar
audit trail --link <link-id> --json`, `planar audit session <session-id>
--json`, or `planar audit commits --task <task-id> --json`. Audit recovery
never claims that state was changed or rolled back.

## Vendor Notes

