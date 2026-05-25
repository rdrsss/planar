---
name: pl-audit-trail
description: For a given external link, show every local session, decision, and commit tied to it.
source: docs/cli-reference.md#domain-audit
---

# Planar Audit Trail (Copilot)

Produces the cross-plane audit trail for an external link — every local session and decision that produced changes to an external ticket.

## What It Does

Starting from an `external_links` link id, reconstructs the full history of local work (sessions, decisions, sync events) tied to that external ticket with timestamps and vendor identity. Provides the inverse view: from Jira or GitHub Issues, trace back to every agent session that touched the ticket.

## CLI Commands

Wraps [`audit trail`](../../docs/cli-reference.md#domain-audit):

```
planar audit trail <link-id>
planar audit session <session-id>
```

## When To Invoke

When an auditor or team member needs to understand what agent work produced a given external ticket state, or when debugging a conflict or unexpected field change on an operational plane ticket.

## Vendor Notes

- Installed to `~/.copilot/skills/pl-audit-trail.md`.
- Companion instruction and prompt files (when needed) live under `copilot/`.
- Active scope and audit state come from the CLI; the skill must not read or write workspace context outside it.
