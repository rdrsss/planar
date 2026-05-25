---
name: pl-handoff
description: Capture a context snapshot before terminating, validate it is resume-ready, and prepare the handoff record.
source: docs/cli-reference.md#domain-handoff
---

# Planar Handoff (Codex)

Implements the end-of-session capture ritual: snapshot the current state, create a handoff record, and validate that the next agent can resume from zero context.

## What It Does

Captures a context snapshot for the current or named task, creates a `handoffs` row in pending status, and automatically validates whether the handoff is resume-ready. Covers the full handoff lifecycle: capture, validate, list, and consume.

## CLI Commands

Wraps [`handoff`](../../docs/cli-reference.md#domain-handoff) and [`capture`](../../docs/cli-reference.md#domain-capture):

```
planar handoff [<task-id>] [--vendor <to-vendor>] [--note <text>]
planar handoff validate <snapshot-id>
planar handoff list [--status <status>] [--task <task-id>]
planar handoff consume <handoff-id>
planar capture session [--task <task-id>] [--vendor <vendor>]
planar capture end [<session-id>] [--summary <text>]
planar capture note <body>
planar capture snapshot [<task-id>] [--note <text>]
```

## When To Invoke

Before terminating an agent process when another session will need to continue the work. Run `capture note` throughout the session to preserve reasoning; run `handoff` at the end to lock the state.

## Vendor Notes

- Installed into `~/.codex/skills/pl-handoff` from `~/.planar/codex-skills/pl-handoff`.
- Active scope and session state come from the CLI; the skill must not read or write workspace context outside it.
- The session id and vendor are recorded on every snapshot created by this workflow.
