---
slug: pl-workbench
description: "Manage the bidirectional workbench filesystem for an active feature."
source: docs/cli-reference.md#domain-workbench
vendor:
  claude:
    argument_hint: "<pull|push|status|resolve|sync|archive|restore|list|publish> [<plan>] [args]"
    invocation_examples: |
      /pl-workbench pull plan:42
      /pl-workbench push plan:42
      /pl-workbench status
      /pl-workbench status plan:42
      /pl-workbench resolve 17 --prefer fs
      /pl-workbench sync plan:42
      /pl-workbench archive plan:42
      /pl-workbench restore plan:42
      /pl-workbench list
      /pl-workbench publish plan:42 --system github
shared_notes:
  - "Workbench reads and writes route through the CLI; the skill must not edit SQLite rows or generated workbench metadata directly."
---

# Planar Workbench ({{.VendorTitle}})

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
| `publish` | DB → external system | Render the workbench files for a plan and push the rendered content to a registered external operational system (Jira, GitHub Issues, GitHub Projects) via the adapter layer. |

## `publish` Subcommand

```
planar workbench publish <plan-id> --system <slug>
```

Renders the workbench files for `<plan-id>` and pushes the rendered content to the named external operational system. The system slug must already be registered (`planar ext list` / `planar ext create`).

| Flag | Description |
|------|-------------|
| `--system <slug>` | External system slug (required). |
| `--json` | Emit a JSON result envelope. |

For full plan-subtree counterpart creation (one external entity per plan / task / artifact, with parent / child links), use `planar ext propagate <plan-id> --system <slug>` — `publish` pushes the rendered Markdown body; `propagate` walks the plan tree.

See [`docs/cli-reference.md#planar-workbench-publish-plan-id`](../../docs/cli-reference.md#planar-workbench-publish-plan-id) for the full specification.

## When To Invoke

Use `pull` after editing workbench files to persist edits into the DB. Use `push` after DB-side changes (task status transitions, plan updates) to refresh on-disk files. Use `sync` for a full reconciliation round. Use `status` to inspect the workbench state before deciding which direction to sync. Use `archive` when a feature is complete and the tree is no longer needed on disk. Use `publish` to push a feature's rendered workbench content to a registered external operational system.

For a higher-level "sync this feature now" action without choosing between verbs, use `pl-workbench-sync`. For archive/restore lifecycle management, use `pl-workbench-archive`.

## Gotchas

- **Push vs pull direction:** `workbench push` writes FROM the database TO
  the filesystem. `workbench pull` writes FROM the filesystem TO the
  database. Confusing the direction causes silent data loss. Always confirm
  which direction you want before running.
- **tech_spec collision on push:** Prior to plan 46 (commit d49d0d2),
  multiple `tech_spec` artifacts on the same plan collided on filename
  `tech-spec.md` during workbench push. The fix uses `<id>-<slug>.md`
  uniformly. Workbenches materialized before plan 46 may have stale
  bare-named files; delete and re-push to clean up.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
