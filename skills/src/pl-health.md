---
slug: pl-health
description: "Report database and handoff readiness health."
source: docs/cli-reference.md#domain-health
vendor:
  claude:
    argument_hint: "[--json]"
    invocation_examples: |
      /pl-health
      /pl-health --json
shared_notes:
  - "Active scope and database state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Health ({{.VendorTitle}})

Reports the operational status of the Planar installation and the handoff readiness of in-flight tasks.

## What It Does

Checks database reachability, schema version currency, SQLite integrity, the count of in-flight tasks that would not pass `resume validate`, and the count of pending handoffs older than the freshness window. Reports overall status as `ok`, `degraded`, or `critical`.

## CLI Commands

Wraps [`health`](../../docs/cli-reference.md#domain-health):

```
planar health [--json]
```

## When To Invoke

At the start of a session to confirm the installation is healthy, in CI to gate on handoff readiness, or when troubleshooting an unexpected error from another command.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
