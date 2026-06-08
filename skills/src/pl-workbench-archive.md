---
slug: pl-workbench-archive
description: "Archive or restore a feature's workbench filesystem tree."
source: docs/cli-reference.md#domain-workbench
vendor:
  claude:
    argument_hint: "<archive|restore> <plan>"
    invocation_examples: |
      /pl-workbench-archive archive plan:<plan-id>
      /pl-workbench-archive restore plan:<plan-id>
      /pl-workbench-archive list
shared_notes:
  - "Archive and restore run through the CLI workbench lifecycle; the skill does not delete workbench trees directly."
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

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
