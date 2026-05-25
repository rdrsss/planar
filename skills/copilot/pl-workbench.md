---
name: pl-workbench
description: Manage the bidirectional workbench filesystem for an active feature.
source: docs/cli-reference.md#domain-workbench
---

# Planar Workbench (Copilot)

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
planar workbench publish <plan-id> --to <path> [--include <glob>]... [--exclude <glob>]... [--dry-run] [--delete-removed]
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
| `publish` | FS → external path | Snapshot the workbench tree to a destination outside `~/.planar/workbench/` (e.g. committing planning docs into a host repo). |

## `publish` Subcommand

```
planar workbench publish <plan-id> --to <path>
```

Copies the on-disk workbench tree for `<plan-id>` to `<path>`. The destination is created if it does not exist. Files with byte-identical content are unchanged; others are (re)written. Options:

| Flag | Description |
|------|-------------|
| `--to <path>` | Destination directory (required). |
| `--include <glob>` | `path.Match` pattern; only matching files published (repeatable, OR semantics). |
| `--exclude <glob>` | `path.Match` pattern; matching files skipped (repeatable, applied after include). |
| `--dry-run` | Preview writes and deletions without modifying the destination. |
| `--delete-removed` | Remove destination files absent from the (filtered) source workbench. |

Human output: per-file `written` / `unchanged` / `deleted` lines followed by a summary count. JSON output (`--json`): `PublishResult` shape with `Written`, `Unchanged`, `Deleted`, `Errors` string arrays. Schema effects: none (read-only). Exit codes: 0 success; 1 user error (missing `--to`, plan not found, source missing or is a regular file, destination is a regular file); 2 destination write failure.

See [`docs/cli-reference.md#planar-workbench-publish-plan-id`](../../docs/cli-reference.md#planar-workbench-publish-plan-id) for the full specification.

## When To Invoke

Use `pull` after editing workbench files to persist edits into the DB. Use `push` after DB-side changes (task status transitions, plan updates) to refresh on-disk files. Use `sync` for a full reconciliation round. Use `status` to inspect the workbench state before deciding which direction to sync. Use `archive` when a feature is complete and the tree is no longer needed on disk. Use `publish` to snapshot a feature's workbench tree to an external path (e.g. a host-repo planning docs directory).

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

- Installed to `~/.copilot/skills/pl-workbench.md`.
- Companion instruction and prompt files (when needed) live under `copilot/`.
- Workbench reads and writes route through the CLI; the skill must not edit SQLite rows or generated workbench metadata directly.
