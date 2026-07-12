---
slug: pl-introspect
description: "Run the usage-introspection pass: mine the diagnostic bundle and local transcripts for friction patterns and file findings on the association's feedback plan."
source: docs/cli-reference.md#domain-report
vendor:
  claude:
    argument_hint: "[--days <n>] [--scope <scope>]"
    invocation_examples: |
      /pl-introspect
      /pl-introspect --days 7
      /pl-introspect --scope assoc:my-org
      /pl-introspect --days 14 --scope assoc:my-org
shared_notes:
  - "The introspector reads only: planar report --json, planar health --json, and local Claude transcript JSONL files. It writes only via planar question add / planar task add on the feedback plan."
  - "Transcript text is ephemeral and never persisted to any entity body. Only aggregate signal (verb path, exit code, retry count) is extracted."
  - "Finding titles are deterministic (<taxonomy-key>: <signal-key>). Re-runs over the same signal are idempotent: title-based dedup skips already-present findings."
---

# Planar Introspect ({{.VendorTitle}})

Run a usage-introspection pass: reads the diagnostic bundle and local vendor
transcripts to identify friction patterns, then files each pattern as a
structured finding on the association's `planar-feedback` plan.

## What It Does

1. Runs `planar report --json --days <n>` to obtain the diagnostic bundle
   (invocation aggregates, failure tail, stale claims, stale handoffs).
2. Mines `~/.claude/projects/**/*.jsonl` for `planar` Bash invocations with
   non-zero exits and retry sequences.
3. Classifies signal into four taxonomy categories: `failure-cluster`,
   `retry-pattern`, `abandoned-workflow`, `gap-feature`.
4. Bootstraps the `planar-feedback` plan (slug `planar-feedback`) in the
   resolved association if it does not exist.
5. For each candidate finding, runs title-based dedup against the feedback
   plan's existing questions and tasks — skips any finding already present.
6. Files new findings via `planar question add` or `planar task add` with
   deterministic signal-derived titles (`<taxonomy-key>: <signal-key>`) and
   redacted-signal bodies (counts, verb paths, error categories — no
   transcript prose, no argument values, no entity text).
7. Reports to the operator: findings filed, findings skipped as duplicates,
   and "nothing noteworthy" if the count is zero.

## Arguments

| Argument | Description | Default |
|----------|-------------|---------|
| `--days <n>` | Window for `planar report --json`. | 30 |
| `--scope <scope>` | Association scope for the feedback plan. | cwd-derived |

## CLI Commands

Reads (via [`report`](../../docs/cli-reference.md#domain-report) and [`plan`](../../docs/cli-reference.md#domain-plan)):

```
planar report --json --days <n>
planar health --json
planar plan list --json [--scope <scope>]
planar plan create "Planar Feedback" --slug planar-feedback [--scope <scope>]
planar question list --plan <id> --status open --json
planar task list --plan <id> --status todo --json
planar task list --plan <id> --status doing --json
```

Writes (per finding, after dedup):

```
planar question add "<taxonomy-key>: <signal-key>" --plan <id> --body "<summary>" [--scope <scope>]
planar task add "<taxonomy-key>: <signal-key>" --plan <id> --body "<summary>" [--scope <scope>]
```

## Privacy

Two contracts hold throughout the pass:

- **Transcript text is never persisted.** Mining extracts only structured
  signal (verb path, exit code, retry count). The prose of the transcript —
  operator messages, assistant responses, tool output — is never written to
  any entity body, file, or table.
- **Finding bodies carry only aggregate signal.** Counts, verb paths,
  timestamps, and error categories from the diagnostic bundle. No entity
  titles, no argument values, no scope slugs.

These contracts are described fully in the [privacy model](../../docs/concepts.md#usage-introspection-privacy-model).

## When To Invoke

- On a regular cadence (weekly or after an intensive agent session) to surface
  accumulated friction before it becomes invisible.
- Before filing a bug report with `pl-report-issue`: run `pl-introspect` first
  to ensure the feedback plan has current findings.
- After a bulk agent dispatch where stale claims or handoffs may have
  accumulated.

## What It Does Not Do

- Does not post to GitHub Issues — use `pl-report-issue` for that.
- Does not modify entity statuses, close tasks, or alter the feedback plan's
  structure beyond adding new rows.
- Does not run with a scheduler or daemon — on-demand only.

## Context

Report the resolved association scope, feedback-plan id when one exists, the
`--days` window, transcript root, available diagnostic sources, and the current
filing mode. Do not expose scope slugs or transcript paths inside finding
bodies.

## Intent

State in one sentence that the run will aggregate in-window friction signal,
deduplicate deterministic findings, and file only new rows on the resolved
feedback plan.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed`. `attempted` counts
candidate findings after classification; `applied` counts newly filed finding
rows plus a newly bootstrapped feedback plan when applicable; `skipped` counts
title-deduplicated candidates; and `failed` counts candidates that could not be
verified or filed. Separately report scanned, malformed, and unavailable
signal-source counts without treating an unavailable optional source as an
applied action.

## Result

Always report `outcome=ok|partial|error`, the stable feedback-plan id, and the
ids and deterministic titles of newly filed questions and tasks. Verify the
post-state with `planar plan show <feedback-plan-id> --json` and the three
canonical question/task list reads before reporting success. A quiet pass is
`outcome=ok` with zero applied, the reason no findings were filed, and no
warning merely for the empty result.

## Warnings

Name unavailable or malformed signal sources, degraded transcript coverage,
post-state reads that could not be completed, and every candidate that failed
after another independent candidate succeeded. Never include transcript
prose, argument values, entity titles observed in transcripts, or scope slugs
in a warning destined for a finding body.

## Next actions

Offer zero to three executable recommendations: inspect the feedback plan,
retry a failed candidate, or pass a filed finding to `pl-report-issue`. Do not
recommend posting externally without the existing operator gate.

## Recovery

For a partial result, retain successfully filed independent findings and give
the original `/pl-introspect` invocation as the idempotent retry; title-based
dedup prevents duplicate rows. Give `planar question list --plan
<feedback-plan-id> --status open --json` and the two task-list reads as exact
inspection commands. Do not claim rollback or offer an undo command: no
cross-finding transaction exists, and completed findings remain filed.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
