---
name: pl-promote
description: Surface personal entities that have matured and promote or demote them between scopes.
source: docs/cli-reference.md#domain-promote
---

# Planar Promote (Codex)

Moves entities between scopes — typically from personal-global to a named association when ad-hoc work turns out to matter.

## What It Does

Promotes a plan, task, question, scenario, artifact, or decision from its current scope to a target association. Once promoted to a workbench-enabled association, the entity is included in the next `workbench export`. `demote` is the symmetric reverse, returning an entity to global personal scope.

## CLI Commands

Wraps [`promote` and `demote`](../../docs/cli-reference.md#domain-promote):

```
planar promote <kind:id> --to <association-slug>
planar demote <kind:id> --to global
```

## When To Invoke

When a task or plan started as personal exploration and has matured enough to belong to an organizational association, or when an entity was promoted prematurely and needs to be pulled back before export.

## Vendor Notes

- Installed into `~/.codex/skills/pl-promote` from `~/.planar/codex-skills/pl-promote`.
- Active scope and entity state come from the CLI; the skill must not read or write workspace context outside it.
