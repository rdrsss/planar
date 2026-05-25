---
name: pl-help
description: Summarize available commands and reference workflows.
source: docs/cli-reference.md#domain-help
---

# Planar Help (Copilot)

Shows the Planar command reference and explains available reference workflows.

## What It Does

Without arguments, lists all top-level subcommands with one-line descriptions. With a subcommand argument, shows that command's full usage including flags, output shape, and exit codes. Equivalent to passing `--help` to any command.

## CLI Commands

Wraps [`help`](../../docs/cli-reference.md#domain-help):

```
planar help [<subcommand> [<sub-subcommand>]]
planar <subcommand> --help
```

## When To Invoke

When exploring what Planar can do, looking up the flags for a specific command, or orienting a new agent session to the available CLI surface.

## Docs Domain

Outward-facing documentation under `docs/` is managed through the
`planar doc` verbs:

- `planar doc lint [--path <dir>]` — validate GFM footnote citations against the structured `references:` front-matter block; flags `undeclared_citation`, `unused_declaration`, `unresolvable_external`, `unresolvable_planar`, and `malformed_entry` issues. Use `--no-refs` to skip external URL HEAD checks.
- `planar doc manifest verify|diff|update|info` — manage `.manifest-docs`, the xxh3-keyed Merkle index across `docs/`. `verify` is the O(1) root compare; `diff` walks the four-signal classifier (regenerate-candidate, hand-edit, new-authoring, deletion); `update` rebuilds and writes atomically; `info <path>` prints one entry's hashes and sources.
- `planar doc promote --kind <k> --source <ref>...` — synthesise a new published doc from named internal artifacts/decisions/plans. The LLM call lives in the `pl-doc-promote` skill; the Go verb writes the body via `--body-file` and updates the manifest.
- `planar doc regenerate (--slug | --path | --all)` — re-synthesise an existing doc whose sources have drifted. Refuses to overwrite hand-edits without `--force` or `--merge`.
- `planar doc backlinks <entity-ref>` — every published doc that cites the given entity in its provenance.
- `planar doc orphans --kind <artifact-kind>` — artifacts of a given kind with zero backlinks in the manifest.
- `planar doc coverage` — `status=done` plans with no published doc that cites them.

See [Features: outward-facing docs system](../../docs/features/doc-system.md) for the full mental model.

## Vendor Notes

- Installed to `~/.copilot/skills/pl-help.md`.
- Companion instruction and prompt files (when needed) live under `copilot/`.
- Active scope and database state come from the CLI; the skill must not read or write workspace context outside it.
