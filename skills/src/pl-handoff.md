---
slug: pl-handoff
description: "Capture a context snapshot before terminating, validate it is resume-ready, and prepare the handoff record."
source: docs/cli-reference.md#domain-handoff
vendor:
  claude:
    argument_hint: "[<task-id>] [--vendor <to-vendor>] [--note <text>]"
    invocation_examples: |
      /pl-handoff 42 --note "Stopped after implementing the API layer; next: write tests"
      /pl-handoff validate 15
      /pl-handoff list
shared_notes:
  - "Active scope and session state come from the CLI; the skill must not read or write workspace context outside it."
  - "The session id and vendor are recorded on every snapshot created by this workflow."
  - "Worktree state (path / branch / repo_root) is NOT yet persisted on `handoffs` or `context_snapshots`. Today the resumer recovers it from the active `agent_work_claims` row — keep the claim alive across the handoff or include the worktree path in the snapshot `body` text so the resumer can `cd` correctly."
---

# Planar Handoff ({{.VendorTitle}})

Implements the end-of-session capture ritual: snapshot the current state, create a handoff record, and validate that the next agent can resume from zero context.

## What It Does

Captures a context snapshot for the current or named task, creates a `handoffs` row in pending status, and automatically validates whether the handoff is resume-ready. Covers the full handoff lifecycle: capture, validate, list, and consume.

## Worktree State And Handoffs

Plan 297 M6 wires the resume packet to surface the `worktree_path` recorded on the active `agent_work_claims` row (per the canonical [worktree](../../agents/methodology.md#worktrees) convention — `epic/<plan-slug>` + `cycle/<plan-slug>/<task-slug>` topology, main checkout stays on master), so a cold-start resumer can prepend `cd <path>` before continuing. The handoff record itself does NOT yet copy that field — there are no `worktree_path` / `branch` columns on `handoffs` or `context_snapshots` today. Two operator-visible consequences:

- If the source session releases its claim before terminating, the resumer's `planar resume` packet will show `active_claim: null` and the worktree context is lost. Keep the claim alive through the handoff (do not call `planar-agent release` until after the resumer has captured the path) or paste the worktree path explicitly into the snapshot body / `--note`.
- Deferred handoff-persistence follow-up: a future iteration adds a `worktree_path` column to either `handoffs` or `context_snapshots` so the resumer can recover the path even when the original claim has been released. Tracked as task 2947 (linked to plan 458 / M6); see [`docs/architecture.md` §Application tables](../../docs/architecture.md#application-tables) for the related `agent_actions.metadata` gap and the broader deferred-schema-work pattern.

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

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
