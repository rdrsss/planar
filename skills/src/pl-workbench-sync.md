---
description: Bidirectionally sync a feature's workbench filesystem with the database.
origin: docs/cli-reference.md#domain-workbench
shared_notes:
    - Active scope and plan state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-workbench-sync
vendor:
    claude:
        argument_hint: <plan> [--prefer fs|db]
        invocation_examples: |
            /pl-workbench-sync plan:<plan-id>
            /pl-workbench-sync checkout-rewrite
---

# Planar Workbench Sync ({{.VendorTitle}})

High-level bidirectional sync for a feature's workbench tree — pull FS edits into the DB, push DB changes back to the FS, and surface any conflicts for explicit resolution.

## What It Does

Runs a full reconciliation round for the named feature without requiring the caller to choose between `pull`, `push`, and `sync` individually. Equivalent to `planar workbench sync <plan>`, which applies FS→DB changes and DB→FS changes in one pass. Conflicts (where both sides changed since the last sync) are written as `sync_events(outcome='conflict')` rows and reported; they require explicit `workbench resolve` to settle — no silent merges occur.

Use this skill when you want a single "make the workbench consistent" action. Use the `pl-workbench` skill directly when you need fine-grained control over direction or need to resolve a specific conflict.

## CLI Commands

Wraps [`workbench`](../../docs/cli-reference.md#domain-workbench):

```
planar workbench pull <plan>
planar workbench push <plan>
planar workbench status [<plan>]
planar workbench resolve <event-id> --prefer fs|db
planar workbench sync <plan>
```

## Recommended Sequence

1. `planar workbench status <plan>` — inspect drift before committing to sync.
2. `planar workbench sync <plan>` — reconcile both directions.
3. If conflicts are reported: `planar workbench resolve <event-id> --prefer fs|db` for each.
4. Re-run `planar workbench status <plan>` to confirm clean state.

## When To Invoke

After the user or another agent has edited workbench files and the DB may also have changed independently. After running `planar task done <id>` or `planar plan update` when the affected plan has a workbench tree. Before handing off to another agent so the receiving agent sees a consistent view.

## Context

Report the resolved scope, plan and workbench path, pre-sync drift, requested
direction or full-sync mode, and any explicit conflict preference.

## Intent

State in one sentence which feature's filesystem and DB state will be inspected
or reconciled.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` per file/entity target,
plus every conflict event ID. Name every failed path or entity with its
direction and failure evidence. Status is read-only; unchanged targets are
skips.

## Result

Always report `outcome=ok|partial|error`. Re-run `planar workbench status
<plan> --json` and return the plan, paths/entities changed, conflict IDs, and
confirmed clean or remaining-drift state. A clean sync is `outcome=ok`, zero
applied, and explains that both sides already match.

## Warnings

Do not resolve conflicts until the operator explicitly chooses `fs` or `db`.
Name remaining conflicts, unavailable post-state, and partial reconciliation.
Successfully synchronized independent targets remain applied; never imply an
automatic rollback.

## Next actions

Give zero to three executable recommendations, normally the exact status read,
then one separately confirmed `planar workbench resolve <event-id> --prefer
fs|db` per conflict, followed by status verification.

## Recovery

For each failed target, give `planar workbench status <plan> --json` and the
exact idempotent `planar workbench sync <plan>` retry. For conflicts, include
the event ID and exact resolve command only after preference confirmation.
Retry does not undo or repeat already-clean targets.

## Vendor Notes

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine (armarium orchestration layer).
