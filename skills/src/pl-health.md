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

## Context

Report that health is global, the resolved database/configuration, and text or
JSON mode. Do not imply that the cwd limits health contributors.

## Intent

State in one sentence that the run is a read-only check of database integrity,
schema currency, handoff readiness, and installed projection health.

## Actions

Report `attempted`, `applied=0`, `skipped`, and `failed` counts for the health
check and its unavailable contributors. Keep the contributor counts returned
by `planar health`; do not recast a degraded contributor as a write failure.

## Result

Always report `outcome=ok|partial|error`, the verified `overall` value, and the
concise non-empty contributor summary from `planar health --json`. A healthy
result explicitly says no reconciliation is needed.

## Warnings

Name degraded or critical contributors and unavailable checks. An expected
zero count or healthy installation emits no warning.

## Next actions

Give zero to three executable recommendations tied to actual contributors,
such as `planar skills status --json`, `planar resume validate <task-id>`, or
invoking `pl-doctor`; omit generic advice when health is clean.

## Recovery

If the check fails, provide `planar health --json` as the exact retry and route
an integrity failure to inspection rather than reconciliation. This read-only
skill has no undo path.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
