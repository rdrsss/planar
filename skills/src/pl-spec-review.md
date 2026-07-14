---
slug: pl-spec-review
description: "Adversarially review draft planning specs for completeness, open questions, feature gaps, user-intent fit, roadmap readiness, and test scenario coverage before ingestion."
source: agents/spec-reviewer.md
model_tier: large
vendor:
  claude:
    argument_hint: "<plan> [--write]"
    invocation_examples: |
      /pl-spec-review <plan-id>
      /pl-spec-review <plan-id> --write
shared_notes:
  - "Default mode is read-only. Writes are limited to operator-approved artifact and question updates; the skill never applies spec ingestion."
---

# Spec Review ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `spec-reviewer` agent.
See [`agents/spec-reviewer.md`](../../agents/spec-reviewer.md) for the full
review contract, verdicts, and write-mode boundary.

## When to use

Invoke after draft specs exist and before `pl-spec-ingest --apply`. This is the
adversarial planning review that asks:

- What are we building?
- Does that match what the user thinks they asked for?
- Are there unresolved questions that block coherent implementation?
- Are product, tech, roadmap, and test artifacts mutually consistent?
- Are features, operational concerns, and test scenarios missing?

Use it after `pl-spec-draft`, `pl-synthesize`, or `pl-import`, and whenever a
draft plan feels plausible but not yet implementation-ready.

## Default mode: read-only review

Read-only review inspects the plan and returns a review packet. It does not
modify artifacts, answer questions, add scenarios, or run ingest apply.

Run the orientation commands:

```
planar scope show
planar plan show <plan> --json
planar artifact list --plan <plan-id> --json
planar question list --plan <plan-id> --json
planar spec ingest <plan> --strict --json
```

Then load each core artifact with:

```
planar artifact show <artifact-id> --json
```

Required artifacts:

- `product_spec`
- `tech_spec`
- `roadmap`
- `test_spec`

If any core artifact is missing, stop with `needs-spec-work` and list the
missing files.

Treat a failed strict preview as evidence, not as a skill failure. The point of
the skill is to find those gaps before the operator applies ingestion. Do not
run `test-spec status` as a pre-ingest completeness check: it reads live rows,
so a draft with no ingested task/scenario rows legitimately has zero totals.

In write mode, compare every approved write target with the cwd-derived scope
from `planar scope show --json`. When they differ, emit
`[cross-scope write: <scope-kind>:<scope-slug>]` as a standalone narrative
line immediately before invoking `planar`, name the actual `project`,
`association`, or `global` target, and pass the matching explicit `--scope`.
Do not emit the cue for same-scope writes. It makes intent visible without
weakening operator approval, strict scope resolution, or binary capabilities.

## Review passes

### 1. Intent reconstruction

Write a concise "what are we building?" paragraph from the specs. Label any
inference that is not directly stated. Compare it to the user's stated goal if
the invocation or surrounding session includes one.

Flag:

- Product goals that do not appear in the roadmap.
- Roadmap work that does not serve a product goal or tech requirement.
- Tech design that implements a different feature than the product spec.
- Non-goals that contradict user-visible acceptance signals.

### 2. Open questions

Read `## Open questions` from every artifact and compare them with
`planar question list --plan <plan-id> --json`.

Classify each question:

- `blocking`: implementation would guess without an answer.
- `non-blocking`: useful, but not needed before ingestion.
- `answered-but-not-reflected`: the question row is answered, but the artifact
  still reads as unresolved.
- `duplicate`: same decision asked in multiple places.

Ask the operator only short, answerable questions. Do not ask the user to solve
the whole design in prose.

### 3. Feature gap analysis

Challenge the draft for missing feature surfaces:

- user roles and permissions
- configuration and defaults
- migrations, backfills, and data retention
- failure modes and recovery
- observability and auditability
- compatibility and rollout
- external-system propagation impact
- docs, install, render, or workflow-surface updates
- cancellation, rollback, and partial-success behavior

Every gap must cite the artifact section that implies the need, or state that
the gap comes from a missing section.

### 4. Roadmap readiness

Use `planar spec ingest <plan> --strict --json` as the mechanical preview. It is
read-only because `--apply` is absent.

Check that:

- roadmap bullets are implementable tasks, not themes
- dependencies are ordered
- testable bullets carry stable `[slug: ...]` annotations
- cross-repo work carries `[touches: ...]` annotations when applicable
- milestones can be reviewed independently
- the previewed task graph matches the feature the specs describe
- `coverage.uncovered_task_slugs`, `coverage.orphan_scenarios`, and top-level
  `slug_collisions` are all empty

Do not apply ingestion.

### 5. Test scenario coverage

For a draft that has not been ingested, use the strict preview's `coverage`
object as the authoritative coverage oracle. A non-zero strict-preview exit or
any non-empty uncovered/orphan/collision array blocks `ready-for-ingest`.

Check that:

- every acceptance signal has at least one scenario or an explicit N/A rationale
- every testable roadmap slug is covered
- scenarios include happy, empty/null, error, and edge paths where applicable
- scenario text is observable behavior, not implementation instructions
- coverage gaps are converted into concrete scenario proposals

For a plan whose ingestion has already been applied, switch to
`planar test-spec status <plan> --json`; that command is the authoritative
post-ingest oracle over live task, scenario, and verifies rows.

## Verdicts

Return exactly one:

- `ready-for-ingest`: no blocking questions or material gaps remain.
- `needs-answers`: operator answers are required before the specs can be made
  coherent.
- `needs-spec-work`: the needed answers are known, but artifacts need edits.
- `abort-replan`: the specs describe the wrong feature or contradict the user's
  intent deeply enough that patching would be dishonest.

## Output packet

```
Verdict: ready-for-ingest | needs-answers | needs-spec-work | abort-replan

Intent read:
<one paragraph>

Blocking questions:
- <question> -- why it blocks implementation

Feature gaps:
- <gap> -- evidence and recommended edit

Consistency gaps:
- <mismatch> -- affected artifacts

Test gaps:
- <gap> -- scenario or N/A rationale needed

Suggested edits:
- <artifact>: <section> -- concrete edit summary

Operator prompts:
1. <short question>
```

If a section has no findings, write `None.` for that section. Do not omit it.

## Write mode

`--write` means "apply operator-approved review resolutions", not "auto-fix
everything." Before writing, show the proposed edits and get explicit approval
in the active conversation.

Allowed writes:

```
planar artifact update <artifact-id> --body @<path>
planar question answer <question-id> --answer "<answer>"
planar question add "<title>" --body "<body>" --plan <plan-id>
```

After writing, rerun:

```
planar spec ingest <plan> --strict --json
```

For an already-ingested plan, also rerun
`planar test-spec status <plan> --json` against the live rows.

Report whether the verdict changed. Do not run `planar spec ingest --apply`.

## Boundaries

- Do not invent answers; ask the operator.
- Do not create implementation tasks directly.
- Do not contact external systems.
- Do not hide material missing behavior as vague future work.
- Keep reviewed project artifacts self-contained. Do not write Planar's own
  internal plan IDs, task IDs, milestone shorthand, or workbench provenance into
  comments or docs unless the operator explicitly asks for cross-project
  provenance.

## Status reporting

See [`agents/spec-reviewer.md` § Status reporting](../../agents/spec-reviewer.md#status-reporting)
for canonical heartbeat strings such as `"loading specs"`,
`"reviewing questions"`, `"reviewing feature gaps"`, and
`"awaiting:operator-answers"`.

The final response keeps the canonical spec-review packet and its
`ready-for-ingest | needs-answers | needs-spec-work | abort-replan` verdict.
The shared fields below wrap that packet; they do not replace its intent read,
gap lists, suggested edits, operator prompts, or strict-preview evidence.

## Context

Report the resolved scope, anchor plan, artifact set, review mode, and whether
coverage came from strict pre-ingest preview or post-ingest live rows.

## Intent

Use the packet's canonical `Intent read` as the interpreted request, making any
inference from artifacts explicit.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for artifact,
question, consistency, and coverage checks. In read-only mode, writes remain
zero; in write mode, count only operator-approved persisted edits as applied.

## Result

Always report `outcome=ok|partial|error`, followed by the authoritative verdict
and complete canonical review packet. Name artifact IDs and the strict-preview
or live-row post-state that supports the verdict.

## Warnings

Name missing artifacts, unresolved questions, degraded evidence, unavailable
verification, and consequential assumptions. Keep blocking gaps in their
canonical packet sections rather than hiding them only as warnings.

## Next actions

Give zero to three executable recommendations, such as answering a numbered
operator prompt, applying an approved spec edit, rerunning
`planar spec ingest <plan> --strict --json`, or proceeding to ingest when ready.

## Recovery

On partial or failed review, provide the exact inspect or idempotent retry
command for the affected plan or artifact. Never run
`planar spec ingest --apply` as recovery and never imply unapproved edits were
rolled back.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
