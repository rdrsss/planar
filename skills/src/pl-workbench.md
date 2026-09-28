---
description: Manage the bidirectional workbench filesystem for an active feature.
origin: docs/cli-reference.md#domain-workbench
shared_notes:
    - Workbench reads and writes route through the CLI; the skill must not edit SQLite rows or generated workbench metadata directly.
slug: pl-workbench
vendor:
    claude:
        argument_hint: <pull|push|status|resolve|sync|archive|restore|list|publish> [<plan>] [args]
        invocation_examples: |
            /pl-workbench pull plan:<plan-id>
            /pl-workbench push plan:<plan-id>
            /pl-workbench status
            /pl-workbench status plan:<plan-id>
            /pl-workbench resolve <conflict-id> --prefer fs
            /pl-workbench sync plan:<plan-id>
            /pl-workbench archive plan:<plan-id>
            /pl-workbench restore plan:<plan-id>
            /pl-workbench list
            /pl-workbench publish plan:<plan-id> --system github
---

# Planar Workbench ({{ VendorTitle }})

Manages the bidirectional sync between the workbench filesystem (`~/.planar/workbench/`) and the Planar database for active features.

## What It Does

The workbench is the drafting surface for agents and users. Each active feature has a directory tree under `$PLANAR_WORKBENCH_ROOT/<association>/<plan-key>-<plan-slug>/` containing Markdown files for plans, tasks, scenarios, decisions, artifacts, and questions. This skill exposes all nine workbench verbs so an agent can read from or write to the DB-backed workbench without touching the database directly.

## CLI Commands

Wraps [`workbench`](../../docs/cli-reference.md#domain-workbench):

```
planar workbench pull <plan>
planar workbench push <plan>
planar workbench status [<plan>]
planar workbench resolve <event-id> --prefer fs|db
planar workbench sync <plan>
planar workbench archive <plan>
planar workbench restore <plan>
planar workbench list
planar workbench publish <plan-id> --system <slug>
```

## Verb Guide

| Verb | Direction | Use when |
|------|-----------|----------|
| `pull` | FS → DB | You or another agent edited files on disk; apply those edits to the DB. |
| `push` | DB → FS | The DB changed (tasks updated, plan status changed); refresh the on-disk files. |
| `status` | read-only | Inspect drift and conflicts without writing. Safe to call any time. |
| `resolve` | settles conflict | `status` or `sync` reported a conflict; pick which side wins (`fs` or `db`). |
| `sync` | both | Full reconciliation — equivalent to pull then push with conflict surfacing. |
| `archive` | removes FS tree | Feature is done; remove the on-disk tree. DB retains everything. |
| `restore` | recreates FS tree | Recreate the tree from the DB after archive, or after accidental deletion. |
| `list` | read-only | List all features that currently have an active FS tree. |
| `publish` | DB → external system | Render the workbench files for a plan and push the rendered content to a registered external operational system (Jira, GitHub Issues) via the adapter layer. |

## `publish` Subcommand

```
planar workbench publish <plan-id> --system <slug>
```

Renders the workbench files for `<plan-id>` and pushes the rendered content to the named external operational system. The system slug must already be registered (`planar-ext ext list` / `planar-ext ext create`).

| Flag | Description |
|------|-------------|
| `--system <slug>` | External system slug (required). |
| `--json` | Emit a JSON result envelope. |

For full plan-subtree counterpart creation (one external entity per plan / task / artifact, with parent / child links), use `planar-ext ext propagate <plan-id> --system <slug>` once it lands — `publish` pushes the rendered Markdown body; `propagate` walks the plan tree. As of this writing `ext propagate` (the whole-feature walk) is not yet implemented on either binary; use `planar-ext ext propagate-one <system> --from <kind:id>` for a single entity today.

See [`docs/cli-reference.md#planar-workbench-publish-plan-id`](../../docs/cli-reference.md#planar-workbench-publish-plan-id) for the full specification.

## When To Invoke

Use `pull` after editing workbench files to persist edits into the DB. Use `push` after DB-side changes (task status transitions, plan updates) to refresh on-disk files. Use `sync` for a full reconciliation round. Use `status` to inspect the workbench state before deciding which direction to sync. Use `archive` when a feature is complete and the tree is no longer needed on disk. Use `publish` to push a feature's rendered workbench content to a registered external operational system.

For a higher-level "sync this feature now" action without choosing between verbs, use `pl-workbench-sync`. For archive/restore lifecycle management, use `pl-workbench-archive`.

## Gotchas

- **Push vs pull direction:** `workbench push` writes FROM the database TO
  the filesystem. `workbench pull` writes FROM the filesystem TO the
  database. Confusing the direction causes silent data loss. Always confirm
  which direction you want before running.
- **tech_spec collision on push:** Older workbenches could materialize
  multiple `tech_spec` artifacts on the same plan as the same filename
  (`tech-spec.md`) during workbench push. The current format uses
  `<id>-<slug>.md` uniformly. Workbenches materialized by older versions may
  have stale bare-named files; delete and re-push to clean up.

## Context

Report the resolved scope, plan and workbench path, verb and direction,
external system for publish, current drift/conflicts, and read, preview, or
mutation mode.

## Intent

State in one sentence which feature tree and DB or external target will be
inspected, reconciled, archived, restored, or published.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` per file, entity, plan,
conflict, or publish target. Name every failed target with plan/path/entity,
direction or system, and failure evidence. Status and list apply zero;
unchanged/idempotent targets are skips.

## Result

Always report `outcome=ok|partial|error`. For filesystem operations, verify
with `planar workbench status <plan> --json` or `planar workbench list --json`
and return paths, entity IDs, and conflict events. For publish, return each
local identity and external result and verify supported link/sync post-state.
A clean status or already-matching operation is an informative no-op.

## Warnings

Preserve direction and conflict gates: confirm pull versus push before a
consequential overwrite, and do not resolve until the operator selects `fs` or
`db`. Name drift, conflicts, unavailable verification, and partial filesystem
or remote results. Never imply atomicity or rollback across independent
targets.

## Next actions

Give zero to three executable recommendations, ordered from status inspection
to a separately confirmed resolve or idempotent retry. A publish result may
recommend its sync-status read, not an automatic propagation.

## Recovery

For every failed target, give `planar workbench status <plan> --json` and the
exact target-specific retry: pull, push, sync, archive, restore, resolve after
preference confirmation, or `planar workbench publish <plan-id> --system
<slug>`. Successful files/entities/remotes remain applied; never fabricate a
cross-target undo.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
