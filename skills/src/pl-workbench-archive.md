---
description: Archive or restore a feature's workbench filesystem tree.
origin: docs/cli-reference.md#domain-workbench
shared_notes:
    - Archive and restore run through the CLI workbench lifecycle; the skill does not delete workbench trees directly.
slug: pl-workbench-archive
vendor:
    claude:
        argument_hint: <archive|restore> <plan>
        invocation_examples: |
            /pl-workbench-archive archive plan:<plan-id>
            /pl-workbench-archive restore plan:<plan-id>
            /pl-workbench-archive list
---

> **Thin wrapper:** this skill is a one-step wrapper over
> `planar workbench archive <plan>` (or `restore`). Shipped to document
> the entry-point shape; use the CLI directly if you don't need the skill
> envelope.

# Planar Workbench Archive ({{.VendorTitle}})

Manages the feature-lifecycle end of the workbench: removes the on-disk tree when a feature is complete, and recreates it on demand from the DB.

## What It Does

When a feature's anchor plan reaches `status='done'` or `status='abandoned'`, the workbench tree under `$PLANAR_WORKBENCH_ROOT` is eligible for archiving. `workbench archive <plan>` removes the FS tree — the database retains every entity row, every relationship, every artifact. `workbench restore <plan>` recreates the tree byte-identically from the DB.

Archive is always an explicit action: Planar does not auto-archive on status change so partially-completed features are not lost to a stray status update.

## CLI Commands

Wraps [`workbench`](../../docs/cli-reference.md#domain-workbench):

```
planar workbench archive <plan>
planar workbench restore <plan>
planar workbench list
```

## When To Invoke

Use `archive` after the feature ships and the on-disk tree is no longer actively needed. Use `restore` to bring the tree back for retrospective editing, re-ingestion, or auditing. Use `list` to see which features currently have an active FS tree (and therefore can be archived).

Before archiving, confirm the tree is in sync with the DB: run `pl-workbench-sync <plan>` first to avoid archiving with unsynced edits.

## Verb Guide

| Verb | Effect |
|------|--------|
| `archive` | Removes the FS tree for the named plan. DB rows are untouched. |
| `restore` | Recreates the FS tree from the DB. Idempotent if the tree already exists and is clean. |
| `list` | Lists active (non-archived) features with FS trees. Shows which plans are eligible for archive. |

## Context

Report the resolved scope, plan, workbench path, lifecycle state, and archive,
restore, or read-only list mode.

## Intent

State in one sentence which feature tree will be listed, archived, or restored.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` per plan tree. Name every
failed plan and path. List applies zero; an already-archived archive or an
already-present clean restore is an expected skip.

## Result

Always report `outcome=ok|partial|error`. After archive or restore, verify the
plan through `planar workbench list --json` and report the plan ID, path, and
observed archived/present state. An empty list or idempotent lifecycle request
is an informative no-op.

## Warnings

Name unsynced drift, ineligible plan status, path verification failures, and
partial multi-plan results. Archive removes only the filesystem tree and must
not be described as deleting DB state.

## Next actions

Give zero to three executable recommendations, led by `planar workbench status
<plan> --json` before archive or the appropriate restore/sync command afterward.

## Recovery

For each failed plan, give `planar workbench status <plan> --json` and the exact
idempotent `planar workbench archive <plan>` or `planar workbench restore
<plan>` retry. A successfully archived or restored independent tree stays in
that state; never imply cross-target rollback.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
