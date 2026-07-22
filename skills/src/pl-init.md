---
description: Initialize the Planar database and register the current directory as a project.
origin: docs/cli-reference.md#domain-init
shared_notes:
    - Resolved scope and database state come from the CLI, not from vendor session memory.
slug: pl-init
vendor:
    claude:
        argument_hint: '[--name <text>] [--db <path>]'
        invocation_examples: |
            /pl-init [--name <text>] [--db <path>]
---

# Planar Init ({{.VendorTitle}})

Initializes a fresh Planar installation for the current working directory.

## What It Does

Applies embedded migrations against the configured SQLite database (creating it if absent), then registers the current working directory as a project. Safe to run more than once — subsequent runs on an already-initialized database are idempotent.

## CLI Commands

Wraps [`planar init`](../../docs/cli-reference.md#domain-init):

```
planar init [--name <text>] [--skip-project] [--allow-no-repo] [--force]
```

## When To Invoke

Run once per machine before any other Planar command. Re-run with `--skip-project` when you want to apply pending migrations without registering a new project.

## Context

Report the cwd, configured database, requested project name, and flags that
change registration or force behavior. Distinguish full initialization from
migration-only mode.

## Intent

State in one sentence whether the request initializes storage, registers the
current repository, or only applies pending migrations.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for database
initialization, migrations, and project registration. An already-current
database or already-registered project is skipped, not failed.

## Result

Always report `outcome=ok|partial|error`. After `planar init`, verify the
post-state with `planar health --json` and, unless `--skip-project` was used,
`planar scope show --json`; return the database health and registered scope.
An idempotent rerun reports zero applied and why nothing changed.

## Warnings

Name forced registration, unavailable repository identity, degraded health, or
failed post-state verification. Expected idempotence is not a warning.

## Next actions

Give zero to three executable recommendations, normally `planar scope show
--json` or the first intended scoped workflow. Omit generic setup advice when
the requested initialization is complete.

## Recovery

On failure, provide `planar health --json` and the exact idempotent `planar init
...` retry with the original flags. Do not claim that applied migrations were
rolled back unless the CLI reports that transaction outcome.

## Vendor Notes

