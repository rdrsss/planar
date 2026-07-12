---
slug: pl-sync
description: "Pull from and push to the operational plane, surface and resolve conflicts."
source: docs/cli-reference.md#domain-sync
vendor:
  claude:
    argument_hint: "<pull|push|status|resolve> <target> [args]"
    invocation_examples: |
      /pl-sync pull task:<task-id>
      /pl-sync push task:<task-id>
      /pl-sync status
      /pl-sync resolve <event-id> --keep local
shared_notes:
  - "Active scope and sync state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Sync ({{.VendorTitle}})

Syncs local entities with registered external operational plane systems (Jira, GitHub Issues).

## What It Does

Pulls remote state into the local plane, pushes local changes to external systems, reports sync status across all links, and resolves conflicts. Sync is always on-demand — no background daemon runs.

## CLI Commands

Wraps [`sync`](../../docs/cli-reference.md#domain-sync):

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar sync pull <link-id | kind:id | --all> [--system <slug>]
planar sync push <link-id | kind:id | --all> [--system <slug>]
planar sync status [--entity <kind:id>] [--system <slug>]
planar sync resolve <event-id> --keep <local|remote>
```

## When To Invoke

At the start of a session to pull the latest remote state, after completing a task to push updates, or when a conflict is reported and needs resolution.

## Context

Report the resolved scope, link/entity/all target, system filter, direction,
status or resolution mode, and requested conflict disposition.

## Intent

State in one sentence which local and remote state will be inspected,
synchronized, or resolved.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` per external link. For
`--all`, name every target by link ID, local entity, and system slug, including
each failed remote call and its evidence. Status is read-only with zero
applied; unchanged links are skips.

## Result

Always report `outcome=ok|partial|error`. Return per-target identities,
direction, sync-event ID/outcome, and confirmed post-state from `planar sync
status --entity <kind:id> --system <slug> --json`. A no-links or already-current
result is `outcome=ok`, zero applied, with the reason. After resolve, verify the
conflict event is closed.

## Warnings

Preserve explicit conflict-resolution intent: do not run `planar sync resolve`
until the operator has confirmed `keep local` or `keep remote`. Name conflicts,
scope mismatches, remote/auth failures, unavailable post-state, and partial
multi-link results. Successful links remain synchronized; no cross-link
rollback is implied.

## Next actions

Give zero to three executable recommendations. Unresolved conflicts lead with
their status evidence and a separately confirmed resolve command; clean
results may recommend the corresponding pull or push only when useful.

## Recovery

For each failed link, give `planar sync status --entity <kind:id> --system
<slug> --json` and the exact idempotent `planar sync pull <link-id>` or `planar
sync push <link-id>` retry with the original system and scope arguments. For a
failed confirmed resolution, retry `planar sync resolve <event-id> --keep
<local|remote>` after inspecting the same event. Never retry successful links
or claim they were undone.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
