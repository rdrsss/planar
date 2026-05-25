---
name: pl-ext-create
description: Create a Jira or GitHub Issues counterpart from a local entity and record the link.
source: docs/cli-reference.md#domain-ext
---

# Planar Ext Create (Copilot)

Creates an operational plane counterpart for a local entity and records the external link.

## What It Does

Takes an existing local entity (plan, task, question, etc.), creates a counterpart on the named external system (Jira, GitHub Issues), and records the link with sync direction. Also supports registering external systems, listing them, testing connectivity, and manually linking or unlinking existing external tickets.

## CLI Commands

Wraps [`ext`](../../docs/cli-reference.md#domain-ext), [`link`, and `unlink`](../../docs/cli-reference.md#domain-link--unlink):

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar ext register jira <slug> --base-url <url> --project <key> --auth-env <var>
planar ext register github <slug> --project <owner/repo> --auth gh-cli
planar ext list
planar ext test <slug>
planar ext create <system-slug> --from <kind:id> [--type <issue-type>] [--role <kind>] [--sync <direction>]
planar link <kind:id> --to <system-slug>:<external-id> [--role <kind>] [--sync <direction>]
planar unlink <link-id>
```

## When To Invoke

When an agent needs to surface a local task or plan to the organizational system of record, or to manually bind an existing Jira ticket or GitHub Issue to a local entity.

## Vendor Notes

- Installed to `~/.copilot/skills/pl-ext-create.md`.
- Companion instruction and prompt files (when needed) live under `copilot/`.
- Active scope and entity state come from the CLI; the skill must not read or write workspace context outside it.
