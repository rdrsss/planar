---
slug: pl-audit-trail
description: "For a given external link, show every local session, decision, and commit tied to it."
source: docs/cli-reference.md#domain-audit
vendor:
  claude:
    argument_hint: "--link <link-id>"
    invocation_examples: |
      /pl-audit-trail --link 7
      /pl-audit-trail --json --link 7
shared_notes:
  - "Active scope and audit state come from the CLI; the skill must not read or write workspace context outside it."
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

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
