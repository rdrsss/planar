---
description: Preview and apply structured triage for redacted feedback findings, with separately gated external reporting.
origin: docs/cli-reference.md#domain-feedback
shared_notes:
    - Preview is mandatory before any local triage mutation; --apply in the initial request is not confirmation.
    - External reporting is a separate workflow with its own full issue preview and explicit confirmation. Declining it preserves completed local triage and posts nothing.
    - Use only redacted evidence and supported planar CLI commands; never open SQLite or persist raw transcript text.
slug: pl-feedback-triage
vendor:
    claude:
        argument_hint: '[--plan <id>] [--finding <task:id|question:id>] [--scope <scope>] [--apply]'
        invocation_examples: |
            /pl-feedback-triage --plan 812
            /pl-feedback-triage --finding question:42
            /pl-feedback-triage --finding task:17 --apply
---

# Planar Feedback Triage ({{ VendorTitle }})

Review redacted findings on a feedback plan, coordinate the feedback-triager's
deterministic assessment, preview every proposed local change, and apply only
the findings the operator explicitly confirms. External reporting is optional
and always routes through the separate mandatory preview in `pl-report-issue`.

## What It Does

1. Resolves the feedback plan and selected `task:<id>` or `question:<id>`
   findings. With no finding filter, reads the plan's findings and current
   structured triage rows.
2. Accepts only redacted evidence: expected and observed behavior, safe
   environment facts, aggregate counts or timestamps, and an optional safe
   reproduction command and result. Raw transcripts, secrets, tokens, local
   paths, argument values, and unrelated entity text are excluded.
3. Coordinates the feedback-triager to assess duplicates by root behavior and
   impact, severity by user impact, reproduction status by observed evidence,
   and a deterministic disposition.
4. Presents a read-only preview for every finding, including evidence gaps,
   proposed fields, durable entity and relationship changes, exact local CLI
   commands, and any external-report recommendation. It then stops for
   explicit confirmation.
5. After confirmation, re-reads affected state, applies only the approved
   local changes, and verifies each entity, relationship, and triage row.
6. If external reporting is requested after local triage, invokes
   `pl-report-issue` for the chosen finding. That workflow must show its own
   complete issue-body preview and receive a second explicit confirmation.
7. After a successful external post and verified linkback, records
   `reported-external` locally through a new triage preview and confirmation;
   it never predicts publication from intent alone.

## Arguments

| Argument | Description | Default |
|----------|-------------|---------|
| `--plan <id>` | Feedback plan whose findings will be reviewed. | resolved `planar-feedback` plan |
| `--finding <kind:id>` | Limit review to one `task:<id>` or `question:<id>`. | all eligible findings |
| `--scope <scope>` | Explicit association scope. | cwd-derived |
| `--apply` | Request application of a reviewed proposal. Still requires explicit confirmation after preview. | off |

## Deterministic Assessment

Use the feedback-triager agent for judgment and preserve its ordering:

1. `duplicate` only for the same root behavior and impact on the same feedback
   plan; title similarity is insufficient. Name the existing triaged target.
2. `accepted` for bounded, actionable behavior that is reproduced or supported
   by authoritative deterministic evidence.
3. `needs-reproduction` for a falsifiable defect with insufficient,
   `not-run`, or `inconclusive` evidence.
4. `retained-question` for an unresolved product or policy choice that is not
   yet a falsifiable defect.
5. `dismissed` only when evidence contradicts the report, behavior is
   supported, or the operator accepts an explicit out-of-scope reason.
6. `reported-external` only after the separate reporting workflow has
   completed and its external link is verified.

Severity is `info|low|medium|high|critical` and describes impact rather than
confidence. Reproduction is `not-run|reproduced|not-reproduced|inconclusive`;
never infer `reproduced` from frequency or severity. Evidence records expected
behavior, observed behavior, safe environment assumptions, and the exact safe
reproduction or inspection command when available.

## Local Preview And Apply Gate

Always produce the complete local preview first, even when the invocation
contains `--apply` or asks to triage immediately. The preview may read current
state but performs no writes. Approval must identify the findings and proposed
severity, disposition, and reproduction fields. Silence, cancellation,
decline, or a request to revise means `applied=0` and no mutation. If evidence
or durable state changes before apply, regenerate the preview and ask again.

For each approved finding, re-read the plan, entity, duplicate target, links,
and triage row. Apply the smallest appropriate local branch:

```text
planar task add "<deterministic actionable title>" --plan <plan-id> --body "<redacted evidence>" [--scope <scope>] --json
planar question add "<deterministic unresolved title>" --plan <plan-id> --body "<redacted evidence>" [--scope <scope>] --json
planar links add task:<task-id> question:<question-id> --relationship addresses --json
planar feedback triage set <task:id|question:id> --severity <value> --disposition <value> --reproduction <value> [--duplicate-of <task:id|question:id>] [--evidence <redacted-text>] [--scope <scope>] --json
```

Reuse an existing task or question when it already represents the finding.
Create an `addresses` relationship only when an accepted task is promoted from
a source question and the relationship does not already exist. Supply
`--duplicate-of` only with `duplicate`, and only for a triaged finding on the
same plan. Local apply never invokes an external API.

## External Reporting Gate

`report-external` is a recommendation, not a local triage disposition and not
permission to publish. After local triage is verified, offer the separate
workflow:

```text
/pl-report-issue --finding <task:id|question:id>
```

`pl-report-issue` must collect its inputs, render the entire issue body, and
stop for a separate explicit confirmation before `gh issue create` or local
linkback. No flag, prior local approval, or request to "apply and report" may
bypass that preview. If the operator declines, no `gh` command runs, no
`external_links` row is created, and the already verified local triage remains
unchanged. Report this as an informative no-op, not a rollback or local
failure.

Only after `pl-report-issue` returns a created issue URL and verified external
link may this skill propose changing the disposition to `reported-external`.
That change requires another local preview and explicit confirmation using
`planar feedback triage set`; never mark it based on an intended, declined, or
failed post.

## CLI Commands

Read during preview, apply recheck, and verification:

```text
planar scope show
planar plan list --json [--scope <scope>]
planar plan show <plan-id> --json
planar task list --plan <plan-id> --json
planar question list --plan <plan-id> --json
planar task show --json <task-id>
planar question show --json <question-id>
planar feedback triage list --plan <plan-id> --json
planar feedback triage show <task:id|question:id> --json
planar links list <task:id|question:id> --json
```

Use only the local mutation commands in the previous section after the local
gate. Use `pl-report-issue`, not a direct `gh` command, for external reporting.
All planning-state access goes through `planar`; never open or edit SQLite.

## Post-State And Partial Results

After every successful local mutation, verify the durable entity with
`task show` or `question show`, the relationship with `links list` when one was
created, and structured state with `feedback triage show`. For multiple
findings, continue independent approved targets when safe. Do not roll back
completed findings because another target fails; return `outcome=partial` and
give an idempotent command for every failed target.

External-post success and local linkback are governed by `pl-report-issue`.
If posting succeeds but linkback fails, preserve the published issue, report
`partial`, and follow that skill's link-only recovery. Do not post again.

## What It Does Not Do

- Does not mine transcripts or create intake findings; use `pl-introspect`.
- Does not write before a complete local preview and explicit confirmation.
- Does not persist raw transcripts, logs, secrets, tokens, paths, arguments,
  or unredacted evidence.
- Does not open SQLite, write agent tables, bypass scope checks, or mutate
  through vendor-specific APIs.
- Does not post externally itself or treat local apply approval as external
  publication approval.
- Does not mark a finding `reported-external` without a verified external link
  and a newly approved local triage update.

## Context

Report the resolved scope, feedback-plan id, selected finding references,
preview or confirmed-apply mode, current triage identities, and whether
external reporting was not requested, recommended, separately previewed,
declined, failed, or completed.

## Intent

State in one sentence which redacted feedback findings will be assessed and
whether the request is for read-only preview, confirmed local apply, or a
separately gated external-report preview after verified local triage.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed`. Count local entity,
relationship, and triage mutations explicitly; preview reads apply zero.
Separate external-report preview, post, and linkback counts from local triage
counts. Name every failed target and whether later independent targets were
attempted. A declined external preview is skipped with no warning and does not
change the completed local counts.

## Result

Always report `outcome=ok|partial|error`, mode, feedback-plan id, final finding
and triage identifiers, severity, disposition, reproduction status,
relationship id when created, and verified post-state. A preview reports the
durable pre-state and proposed commands with `applied=0`. If external reporting
was completed, include the verified issue URL and external-link id. If it was
declined, state that nothing was posted and the local triage post-state was
preserved.

## Warnings

Name redaction or evidence gaps, changed-state refusals, invalid duplicate
targets, failed verification, partial local application, and external post or
linkback failures. State clearly when external reporting was merely
recommended and not performed. Never include raw transcript prose, secrets,
tokens, local paths, unredacted arguments, or unsupported rollback claims.

## Next actions

Offer zero to three executable recommendations. A local preview leads with the
explicit confirmation choice. Verified local triage may offer
`/pl-report-issue --finding <kind:id>` as a separately gated next workflow.
After apply, prefer exact `feedback triage show`, entity inspection, or
idempotent retry commands over generic advice.

## Recovery

For a cancelled or declined local preview, report zero writes and rerun the
original `/pl-feedback-triage` invocation to regenerate it. For a partial local
apply, preserve completed independent findings and inspect each target with
`planar feedback triage show <task:id|question:id> --json`; rerun the original
skill invocation to re-preview only failed or skipped work. Inspect promoted
relationships with `planar links list <task:id|question:id> --json`.

For a declined external preview, no recovery is required: nothing was posted,
no external link was written, and local triage remains durable. Rerun
`/pl-report-issue --finding <task:id|question:id>` only when the operator wants
a fresh full-body preview. If posting succeeded but linkback failed, do not
post again; follow `pl-report-issue` recovery to retry only the exact
`planar link` command and verify it before proposing `reported-external`.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
