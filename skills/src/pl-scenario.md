---
slug: pl-scenario
description: "Author test scenarios from a spec or task, verify and record outcomes."
source: docs/cli-reference.md#domain-scenario
vendor:
  claude:
    argument_hint: "<add|verify|list|show|retire> [args]"
    invocation_examples: |
      /pl-scenario add "Stripe webhook idempotency" --related 3
      /pl-scenario verify 9 --outcome pass
      /pl-scenario list --status failing
shared_notes:
  - "Resolved scope and scenario state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Scenario ({{.VendorTitle}})

Manages test scenarios — verification artifacts tied to specs, plans, or tasks.

## What It Does

Creates test scenarios with descriptions and optional artifact links, records verification outcomes (pass, fail, error, skipped), and lists scenarios by status. Planar records scenario outcomes; it does not execute them.

## CLI Commands

Wraps [`scenario`](../../docs/cli-reference.md#domain-scenario):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. The active scope stack was removed in plan 153 M5; there is no `scope use` to push.

```
planar scenario add <title> [--body <text>] [--related <artifact-id>] [--scope <scope>]
planar scenario verify <scenario-id> --outcome <pass|fail|error|skipped> [--notes <text>]
planar scenario list [--scope <scope>] [--status <status>]
planar scenario show <scenario-id>
planar scenario retire <scenario-id>
```

## When To Invoke

When authoring a tech spec or design note and wanting to record what must be verified, or after running tests to commit the outcome to the local plane for handoff continuity.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
