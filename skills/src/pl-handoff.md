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
  - "Worktree state (path / branch / repo_root) is persisted on the handoff row at create time, copied from the active `agent_work_claims` row on the target task. `planar resume` reads it from the live claim when one exists, and falls back to the most-recent worktree-bearing handoff once the claim has been released — operators do not need to keep the claim alive across the handoff."
---

# Planar Handoff ({{.VendorTitle}})

Implements the end-of-session capture ritual: snapshot the current state, create a handoff record, and validate that the next agent can resume from zero context.

## What It Does

Captures a context snapshot for the current or named task, creates a `handoffs` row in pending status, and automatically validates whether the handoff is resume-ready. Covers the full handoff lifecycle: capture, validate, list, and consume.

## Worktree State And Handoffs

`planar handoff` automatically captures the active claim's worktree context onto the handoff row at create time. The fields persisted — `worktree_path`, `repo_root`, `branch` — mirror the worktree-isolated topology (`epic/<plan-slug>` + `cycle/<plan-slug>/<task-slug>`, main checkout stays on master), whether the worktree was created for a sequential worktree cycle or a `parallel-fanout` lane. When no claim is held on the target task at handoff time, the columns are left NULL and no fallback path is created — that's the legacy / no-isolation flow.

`planar resume <task>` reads the worktree context from two sources, in priority order:

1. **Live claim** — when an `agent_work_claims` row with status `active` exists for the task, its `worktree_path` is surfaced as `active_claim.worktree_path` in JSON and as the `worktree:` / `cd:` lines in the text packet's audit footer.
2. **Handoff fallback** — when no active claim exists (or the active claim row has a NULL `worktree_path`), the resumer reads the most-recent non-abandoned handoff for the task with a non-null `worktree_path`. The recovered fields surface as `from_handoff.worktree_path` in JSON and as the `from handoff: <id>` block in the text packet's audit footer.

The fallback is the cold-start recovery path: a session that captured a handoff and then released its claim can still be resumed from a fresh process. Operators no longer need to keep the claim alive across the handoff.

## CLI Commands

Wraps [`handoff`](../../docs/cli-reference.md#domain-handoff) and [`capture`](../../docs/cli-reference.md#domain-capture):

```
planar handoff [<task-id>] [--vendor <to-vendor>] [--note <text>]
planar handoff validate <snapshot-id>
planar handoff list [--status <status>]
planar handoff consume <handoff-id>
planar capture session [--task <task-id>] [--vendor <vendor>]
planar capture end [<session-id>] [--summary <text>]
planar capture note <body>
planar capture snapshot [<task-id>] [--note <text>]
```

## When To Invoke

Before terminating an agent process when another session will need to continue the work. Run `capture note` throughout the session to preserve reasoning; run `handoff` at the end to lock the state.

## Context

Report the resolved scope, task, session, source and destination vendor,
worktree/branch source, and capture, validate, list, consume, or create mode.

## Intent

State in one sentence which session or task continuity record will be captured,
inspected, validated, or consumed.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` across session snapshot,
handoff, validation, and consumption targets. Name every failed task, snapshot,
or handoff ID. List and validate are read-only; an already-consumed handoff or
an unchanged capture is an explained skip.

## Result

Always report `outcome=ok|partial|error`. After creation, read `planar handoff
list --status pending --json` and return the handoff and snapshot IDs,
resume-ready state, task, vendor, worktree, and branch. After consume, verify
that the handoff is consumed. A list with no matches is an informative no-op.

## Warnings

Name validation gaps, absent claim/worktree evidence, unavailable post-state,
and partial capture where a snapshot exists but the handoff is not
resume-ready. Do not treat a legitimate legacy null worktree as a warning
unless it prevents the requested resume path.

## Next actions

Give zero to three executable recommendations, normally `planar handoff
validate <snapshot-id>`, `planar resume validate <task-id>`, or the precise
capture remediation reported by validation.

## Recovery

For each failed target, give its exact inspection and idempotent retry:
`planar handoff validate <snapshot-id>`, `planar handoff [<task-id>] --vendor
<vendor> --note <text>`, or `planar handoff consume <handoff-id>`. A persisted
snapshot or handoff remains persisted; never claim the other targets rolled it
back.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
