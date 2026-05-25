---
name: pl-init
description: Initialize the Planar database and register the current directory as a project.
source: docs/cli-reference.md#domain-init
---

# Planar Init (Copilot)

Initializes a fresh Planar installation for the current working directory.

## What It Does

Applies embedded migrations against the configured SQLite database (creating it if absent), then registers the current working directory as a project. Safe to run more than once — subsequent runs on an already-initialized database are idempotent.

## CLI Commands

Wraps [`planar init`](../../docs/cli-reference.md#domain-init):

```
planar init [--name <text>] [--db <path>] [--skip-project]
```

## When To Invoke

Run once per machine before any other Planar command. Re-run with `--skip-project` when you want to apply pending migrations without registering a new project.

## Vendor Notes

- Installed to `~/.copilot/skills/pl-init.md`.
- Companion instruction and prompt files (when needed) live under `copilot/`.
- Resolved scope and database state come from the CLI, not from vendor session memory.
