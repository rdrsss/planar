---
description: Create a Jira or GitHub Issues counterpart from a local entity and record the link.
origin: docs/cli-reference.md#domain-ext
shared_notes:
    - Active scope and entity state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-ext-create
vendor:
    claude:
        argument_hint: <system-slug> --from <kind:id> [--type <issue-type>]
        invocation_examples: |
            /pl-ext-create acme-jira --from plan:<plan-id> --type Epic
            /pl-ext-create side-gh --from task:<task-id>
---

# Planar Ext Create ({{.VendorTitle}})

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
planar ext register github <slug> --project <owner/repo> [--auth-env <var>]
planar ext list
planar ext test <slug>
planar ext create <system-slug> --from <kind:id> [--type <issue-type>] [--role <kind>] [--sync <direction>]
planar link <kind:id> --to <system-slug>:<external-id> [--role <kind>] [--sync <direction>]
planar unlink <link-id>
```

## When To Invoke

When an agent needs to surface a local task or plan to the organizational system of record, or to manually bind an existing Jira ticket or GitHub Issue to a local entity.

## Context

Report the resolved scope, local entity, external system, requested role and
sync direction, and whether the request is a connectivity read, create, link,
unlink, or registration mutation.

## Intent

State in one sentence which local and external identities will be connected or
inspected.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` counts per system and
entity. Name every failed target with its local identity, system slug, and
available remote evidence. An already-existing exact binding is a skip.

## Result

Always report `outcome=ok|partial|error`. After create or link, verify with
`planar sync status --entity <kind:id> --system <system-slug> --json` and return
the local entity, external link ID, system slug, and external URL or ID. After
unlink, verify that the link is absent. List and test are read-only with zero
applied; an empty list is an informative no-op.

## Warnings

Name scope mismatches, failed connectivity, ambiguous or duplicate bindings,
unavailable post-state, and independent remote calls that partially succeed.
Never imply that a successfully created remote target was rolled back.

## Next actions

Give zero to three executable recommendations, led by the verified link's
status read or the next gated propagation preview when appropriate.

## Recovery

For each failed target, give `planar sync status --entity <kind:id> --system
<system-slug> --json` and the exact idempotent `planar ext create ...` or
`planar link ...` retry with the original role, sync, and scope arguments.
Inspect an unlink failure with `planar audit trail --link <link-id> --json`.
Completed independent targets remain applied.

## Vendor Notes

