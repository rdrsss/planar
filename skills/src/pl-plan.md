---
slug: pl-plan
description: "Draft a plan from a goal, decompose into steps, link to specs and ADRs."
source: docs/cli-reference.md#domain-plan
vendor:
  claude:
    argument_hint: "<create|show|list|step|link> [args]"
    invocation_examples: |
      /pl-plan create "Implement billing module"
      /pl-plan show <plan-id>
      /pl-plan step add <plan-id> "Design the data model"
      /pl-plan link <plan-id> artifact:<artifact-id> --relationship cites
shared_notes:
  - "Resolved scope and plan state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Plan ({{.VendorTitle}})

Manages plans — the top-level structured intent for a body of work.

## What It Does

Creates plans from a goal statement, decomposes them into ordered steps, tracks step progress, and links plans to related artifacts, decisions, and other plans via `entity_links`.

## CLI Commands

Wraps [`plan`](../../docs/cli-reference.md#domain-plan):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar plan create <title> [--scope <scope>] [--parent <plan-id>] [--summary <text>]
planar plan show <plan-id>
planar plan list [--scope <scope>] [--status <status>]
planar plan update <plan-id> [--title <text>] [--status <status>]
planar plan step add <plan-id> <body>
planar plan step done <step-id>
planar plan step skip <step-id>
planar plan step link <step-id> <task-id>
planar plan link <plan-id> <to-kind:to-id> --relationship <kind>
```

## When To Invoke

When starting a significant body of work that benefits from a structured decomposition — new feature, migration, refactor, or multi-session investigation.

## Context

Report the resolved scope, plan or step target, requested operation, parent or
relationship target, and read or mutation mode.

## Intent

State in one sentence whether the request creates, inspects, updates,
decomposes, advances, or links a plan.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for plans, steps,
and links. Read-only list/show operations have zero applied; already-satisfied
step state or existing links are expected skips when the CLI reports them so.

## Result

Always report `outcome=ok|partial|error`. After every create, update, step, or
link mutation, read `planar plan show <plan-id> --json` and return the stable
plan ID, status, affected step or relationship, and verified post-state. A
successful no-op reports zero applied and its reason.

## Warnings

Name missing or ambiguous targets, scope mismatch, invalid transitions,
unavailable post-state reads, and partial independent mutations. An empty list
or expected idempotent state is not a warning.

## Next actions

Give zero to three executable recommendations tied to the verified state, such
as `planar plan show <plan-id> --json` or the next step command.

## Recovery

For a failed target, provide `planar plan show <plan-id> --json` and the exact
idempotent retry using the original `--scope`, parent, status, step, or
relationship arguments. Never imply that an independently created plan, step,
or link was rolled back.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
