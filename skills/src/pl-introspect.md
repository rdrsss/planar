---
slug: pl-introspect
description: "Preview redacted usage-introspection findings, report signal coverage, and apply approved findings to the association's feedback plan."
source: docs/cli-reference.md#domain-report
vendor:
  claude:
    argument_hint: "[--days <n>] [--scope <scope>] [--apply]"
    invocation_examples: |
      /pl-introspect
      /pl-introspect --days 7
      /pl-introspect --scope assoc:my-org
      /pl-introspect --days 14 --scope assoc:my-org
      /pl-introspect --days 14 --scope assoc:my-org --apply
shared_notes:
  - "Default invocation is a read-only preview. --apply requires explicit operator confirmation and writes only the feedback plan plus approved, deduplicated question/task findings."
  - "Transcript text is ephemeral and never persisted to any entity body. Only aggregate signal (verb path, exit code, retry count) is extracted."
  - "Finding titles are deterministic (<taxonomy-key>: <signal-key>). Re-runs over the same signal are idempotent: title-based dedup skips already-present findings."
---

# Planar Introspect ({{.VendorTitle}})

Run a usage-introspection pass: read the diagnostic bundle and available local
vendor signal, identify friction patterns, and preview structured findings.
Default invocation writes nothing. After reviewing the preview, the operator
may explicitly confirm `--apply` to file approved findings on the association's
`planar-feedback` plan.

## What It Does

1. Runs `planar report --json --days <n>` to obtain the diagnostic bundle
   (invocation aggregates, failure tail, stale claims, stale handoffs).
2. Resolves configured Claude, Codex, Copilot, and CLI-log adapters, then
   normalizes recognized records into the same redacted signal shape.
3. Classifies signal into four taxonomy categories: `failure-cluster`,
   `retry-pattern`, `abandoned-workflow`, `gap-feature`.
4. Reports every considered source as `observed`, `unavailable`, or `disabled`;
   an observed empty source is distinct from a source that was not read.
5. Presents the complete redacted candidate preview and performs no writes.
6. Only after explicit confirmation with `--apply`, bootstraps the
   `planar-feedback` plan (slug `planar-feedback`) if it does not exist.
7. For each approved candidate finding, runs title-based dedup against the feedback
   plan's existing questions and tasks — skips any finding already present.
8. Files new findings via `planar question add` or `planar task add` with
   deterministic signal-derived titles (`<taxonomy-key>: <signal-key>`) and
   redacted-signal bodies (counts, verb paths, error categories — no
   transcript prose, no argument values, no entity text).
9. Reports attempted, applied, skipped, and failed counts, plus coverage and
   whether an empty result was actually observed.

## Arguments

| Argument | Description | Default |
|----------|-------------|---------|
| `--days <n>` | Window for `planar report --json`. | 30 |
| `--scope <scope>` | Association scope for the feedback plan. | cwd-derived |
| `--apply` | Apply the reviewed proposal after explicit confirmation. | off (read-only preview) |

## Preview And Apply Gate

Always run and present the preview first, even when the request mentions
`--apply`. The preview may read an existing feedback plan to predict duplicate
skips, but it must not bootstrap a plan or add findings. Show the deterministic
titles, finding kinds, aggregate bodies, duplicate predictions, and
`signal_coverage`, then stop for explicit operator confirmation.

Only an affirmative confirmation authorizes apply. Cancellation, silence, or a
request to revise the candidate set returns the preview with zero applied and
zero writes. Apply must use the approved candidate set, recheck dedup immediately
before each add, and limit writes to the commands listed below. Do not treat
`--apply` text in an unconfirmed request as confirmation.

## CLI Commands

Reads in preview and apply (via [`report`](../../docs/cli-reference.md#domain-report) and [`plan`](../../docs/cli-reference.md#domain-plan)):

```
planar report --json --days <n>
planar health --json
planar config show --effective --json
planar plan list --json [--scope <scope>]
planar question list --plan <id> --status open --json
planar task list --plan <id> --status todo --json
planar task list --plan <id> --status doing --json
```

Writes only after the explicit `--apply` gate (bootstrap when absent, then per
approved finding after dedup):

```
planar plan create "Planar Feedback" --slug planar-feedback [--scope <scope>]
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
- **Coverage is redacted.** Unavailable-source warnings identify the adapter
  kind and a safe reason, never raw prose, arguments, entity text, scope slugs,
  or local transcript paths.
- **Normalized evidence is bounded.** Adapter output contains only vendor,
  verb path, closed category, aggregate count, and first/last timestamps.

## Adapter Configuration

Read configuration through `planar config show --effective --json`. Under
`[introspection.transcripts]`, each vendor has `*_enabled` and `*_path` keys.
Disabled wins; then a non-empty override; then the built-in Claude
`~/.claude/projects/**/*.jsonl`, Codex `~/.codex/sessions/**/*.jsonl`, or
Copilot `~/.copilot/session-state/**` location. Never fall back after an
explicit override fails. CLI-log comes only from `planar report --json` and is
disabled when `[introspection].cli_log` is false.

Report `scanned`, `malformed`, and `normalized` per adapter. Unknown versions
and malformed records increment `malformed` but never enter evidence or warning
text. Deduplicate by vendor, verb path, category, and hour bucket; matching
CLI-log invocations suppress transcript duplicates because CLI-log is
authoritative for invocations it contains.

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
- Does not write during preview, infer confirmation, write feedback-triage
  metadata, or invoke vendor-specific mutation APIs.

## Context

Report the resolved association scope, feedback-plan id when one exists, the
`--days` window, transcript root, available diagnostic sources, current mode
(`preview` or confirmed `apply`), and `signal_coverage`. Do not expose scope
slugs or transcript paths inside finding bodies.

## Intent

State in one sentence that the run will aggregate in-window friction signal and
preview deterministic findings; in confirmed apply mode, it will file only the
approved new rows on the resolved feedback plan.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed`. `attempted` counts
candidate findings after classification; `applied` counts newly filed finding
rows plus a newly bootstrapped feedback plan when applicable; `skipped` counts
title-deduplicated candidates; and `failed` counts candidates that could not be
verified or filed. Separately report scanned, malformed, and unavailable
signal-source counts without treating an unavailable optional source as an
applied action.

## Result

Always report `outcome=ok|partial|error`, mode, proposals, and
`signal_coverage`. In apply mode also report the stable feedback-plan id and the
ids and deterministic titles of newly filed questions and tasks. Verify apply
post-state with `planar plan show <feedback-plan-id> --json` and the three
canonical question/task list reads before reporting success. A quiet pass is
`outcome=ok` with zero applied and no warning only when every enabled source
needed for that conclusion was observed and returned zero candidates. If no
source was observed, do not claim a quiet success: use `partial` when useful
analysis remains or `error` when none was possible.

## Warnings

Name unavailable, disabled, or malformed signal sources, degraded transcript coverage,
post-state reads that could not be completed, and every candidate that failed
after another independent candidate succeeded. Never include transcript
prose, argument values, entity titles observed in transcripts, or scope slugs
in a warning destined for a finding body.

## Next actions

Offer zero to three executable recommendations. A preview with proposals leads
with the exact `/pl-introspect ... --apply` invocation, explicitly subject to
operator confirmation. An applied result may recommend inspecting the feedback
plan, retrying a failed candidate, or passing a filed finding to
`pl-report-issue`. Do not recommend posting externally without the existing
operator gate.

## Recovery

For a cancelled preview, report zero writes and rerun the original invocation
to regenerate the proposal. For a partial apply, retain successfully filed
independent findings and give the original `/pl-introspect` invocation as the idempotent retry; title-based
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
