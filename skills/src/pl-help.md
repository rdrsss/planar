---
slug: pl-help
description: "Summarize available commands and reference workflows."
source: docs/cli-reference.md#domain-help
vendor:
  claude:
    argument_hint: "[<subcommand>]"
    invocation_examples: |
      /pl-help
      /pl-help task
      /pl-help sync resolve
shared_notes:
  - "Active scope and database state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Help ({{.VendorTitle}})

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
dedicated `planar-doc` binary (it touches the working tree and the
`.planar-manifest` Merkle index, never SQLite):

- `planar-doc build` — recompute the manifest from the current working tree.
- `planar-doc verify` — compare the stored manifest root against the current tree (the O(1) root compare).
- `planar-doc diff` — list drift records via the drift classifier (regenerate-candidate / hand-edit / new-authoring / deletion / nodoc-stale).
- `planar-doc cover <path>` — add or remove a source path on a doc entry.
- `planar-doc nodoc <path>` — add or remove a path in the nodoc map.
- `planar-doc lint [--path <dir>]` — validate GFM footnote citations against the structured `references:` front-matter block; flags `undeclared_citation`, `unused_declaration`, `unresolvable_external`, `unresolvable_planar`, and `malformed_entry` issues.
- `planar-doc schema` — print the `planar-doc` command tree as a JSON catalog.

> The synthesis verbs (`promote`, `regenerate`, `backlinks`, `orphans`,
> `coverage`) are from the Go implementation and are **not yet wired into
> the Zig port** — the engine lives under `src/engine/docs/` but has no CLI
> entry point yet.

See [Features: outward-facing docs system](../../docs/features/doc-system.md) for the full mental model.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
