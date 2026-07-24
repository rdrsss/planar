---
description: Author test scenarios from a spec or task, verify and record outcomes.
origin: docs/cli-reference.md#domain-scenario
shared_notes:
    - Resolved scope and scenario state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-scenario
vendor:
    claude:
        argument_hint: <add|verify|list|show|retire> [args]
        invocation_examples: |
            /pl-scenario add "Stripe webhook idempotency" --related 3
            /pl-scenario verify 9 --outcome pass
            /pl-scenario list --status failing
---

# Planar Scenario ({{.VendorTitle}})

Manages test scenarios — verification artifacts tied to specs, plans, or tasks.

## What It Does

Creates test scenarios with descriptions and optional artifact links, records verification outcomes (pass, fail, error, skipped), and lists scenarios by status. Planar records scenario outcomes; it does not execute them.

## CLI Commands

Wraps [`scenario`](../../docs/cli-reference.md#domain-scenario):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

```
planar scenario add <title> [--body <text>] [--related <artifact-id>] [--scope <scope>]
planar scenario verify <scenario-id> --outcome <pass|fail|error|skipped> [--summary <text>]
planar scenario list [--scope <scope>] [--status <status>]
planar scenario show <scenario-id>
planar scenario retire <scenario-id>
```

## When To Invoke

When authoring a tech spec or design note and wanting to record what must be verified, or after running tests to commit the outcome to the local plane for handoff continuity.

## Context

Report the resolved scope, scenario target or filter, operation, related
artifact when present, and read or mutation mode.

## Intent

State in one sentence whether the request creates, inspects, verifies, lists,
or retires scenario evidence.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for scenario rows,
verification outcomes, and artifact relationships. For list operations,
`applied` is zero and the result count is reported separately. Name every
failed scenario target.

## Result

Always report `outcome=ok|partial|error`. After `add`, `verify`, or `retire`,
read every affected row with `planar scenario show <scenario-id> --json` and
return its stable ID, status, related artifact, and latest recorded outcome.
List and show return the matching verified rows. A repeated operation that
already has the requested post-state reports `outcome=ok`, zero applied, and
the reason it was a no-op.

## Warnings

Name missing or ambiguous targets, unsupported transitions, unavailable
post-state reads, and partial multi-scenario results. An empty list or expected
idempotent no-op is not itself a warning.

## Next actions

Give zero to three executable recommendations, such as `planar scenario show
<scenario-id> --json`, the next verification command, or a filtered `planar
scenario list --status <status>`.

## Recovery

For each failed target, give its exact inspection command and idempotent retry:
`planar scenario show <scenario-id> --json`, `planar scenario verify
<scenario-id> --outcome <outcome> [--summary <text>]`, or `planar scenario
retire <scenario-id>`. Completed independent scenarios remain changed; do not
claim a cross-scenario rollback.

## Vendor Notes

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine (armarium orchestration layer).
