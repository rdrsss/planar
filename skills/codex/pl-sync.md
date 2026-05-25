---
name: pl-sync
description: Pull from and push to the operational plane, surface and resolve conflicts.
source: docs/cli-reference.md#domain-sync
---

# Planar Sync (Codex)

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
planar sync pull <link-id | kind:id | --all> [--since <window>] [--system <slug>]
planar sync push <link-id | kind:id | --all> [--system <slug>]
planar sync status [--scope <scope>] [--system <slug>]
planar sync resolve <event-id> --keep <local|remote>
```

## When To Invoke

At the start of a session to pull the latest remote state, after completing a task to push updates, or when a conflict is reported and needs resolution.

## Vendor Notes

- Installed into `~/.codex/skills/pl-sync` from `~/.planar/codex-skills/pl-sync`.
- Active scope and sync state come from the CLI; the skill must not read or write workspace context outside it.
