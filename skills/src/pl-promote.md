---
description: Surface personal entities that have matured and promote or demote them between scopes.
origin: docs/cli-reference.md#domain-promote
shared_notes:
    - Active scope and entity state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-promote
vendor:
    claude:
        argument_hint: <kind:id> --to <association-slug> | demote <kind:id> [--from <association-slug>]
        invocation_examples: |
            /pl-promote task:<task-id> --to org:acme
            /pl-promote plan:<plan-id> --to project:billing
            /pl-promote demote task:<task-id>
---

# Planar Promote ({{.VendorTitle}})

Moves entities between scopes — typically from personal-global to a named association when ad-hoc work turns out to matter.

## What It Does

Promotes a plan, task, question, scenario, artifact, or decision from its current scope to a target association. Once promoted to a workbench-enabled association, the entity is included in the next `workbench export`. `demote` is the symmetric reverse, returning an entity to global personal scope.

## CLI Commands

Wraps [`promote` and `demote`](../../docs/cli-reference.md#domain-promote):

```
planar promote <kind:id> --to <association-slug>
planar demote <kind:id> [--from <association-slug>]
```

## When To Invoke

When a task or plan started as personal exploration and has matured enough to belong to an organizational association, or when an entity was promoted prematurely and needs to be pulled back before export.

## Context

Report the entity kind and ID, current scope, target or source association, and
promote or demote mode before writing.

## Intent

State in one sentence which durable entity relationship to organizational
scope the operator wants to change.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts per entity. An
entity already in the requested scope is an informative skip, not a failure.

## Result

Always report `outcome=ok|partial|error`. After mutation, run `planar <kind>
show <id> --json` and return the stable `kind:id` plus its verified scope. A
successful no-op reports zero applied and the already-matching scope.

## Warnings

Name unsupported entity kinds, ambiguous associations, export consequences,
scope mismatches, and unavailable post-state reads. Expected idempotence emits
no warning.

## Next actions

Give zero to three executable recommendations tied to the new scope, such as
the exact entity show or relevant workbench inspection command.

## Recovery

Provide `planar <kind> show <id> --json` and the exact idempotent promote or
demote retry. Offer the real inverse command only when the operator asks to
reverse a verified applied move; do not imply automatic rollback.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
