---
description: Draft initial product spec, tech spec, test spec, and roadmap for a new feature from a goal statement.
origin: agents/planner.md
shared_notes:
    - Active scope and plan state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-spec-draft
vendor:
    claude:
        argument_hint: '"<goal>"'
        invocation_examples: |
            /pl-spec-draft "rebuild the checkout flow to support multi-currency"
            /pl-spec-draft "add real-time notifications to the dashboard"
---

# Spec Draft ({{ VendorTitle }})

{{ VendorTitle }} skill surface for the vendor-neutral `planner` agent. See [`agents/planner.md`](../../agents/planner.md) for the full role spec, input/output contract, doc shape conventions, and the **four-phase authoring discipline** (product → tech → roadmap → test). The phase-specific "do NOT" lists are load-bearing: they keep product-spec out of implementation, keep test-spec out of code, and ensure the four return-path buckets (happy / empty-null / error / edge) are reasoned through explicitly as a coverage lens — not collapsed into document structure.

Phase 4 has a self-check before final emission (see [`agents/planner.md` §Phase 4 self-check](../../agents/planner.md#phase-4-self-check-before-final-emission)): every scenario has a non-empty `**Verifies:**`, every cited slug exists as a `[slug: …]` annotation on a roadmap bullet, and every testable bullet carries a `[slug: …]`. Run the read-only strict JSON preview, `planar spec ingest <plan> --strict --json`, before handoff. Its workbench-derived `coverage` object is authoritative while the plan is still a draft; `planar test-spec status` is reserved for post-ingest live rows.

The draft fails self-check if the strict preview exits non-zero or reports a
non-empty `coverage.uncovered_task_slugs`, `coverage.orphan_scenarios`, or
top-level `slug_collisions` array. An empty pre-ingest `test-spec status` result
is not evidence of coverage.

## When to use

Invoke at the very start of a new feature, before any tasks exist. The user states a goal; this skill produces **four** reviewable planning documents and registers them as artifacts in the database. The documents are deliberately drafts — the user reads and edits them before invoking `pl-spec-ingest` to decompose them into tasks.

Do **not** invoke this skill to create tasks. Task creation belongs to `pl-spec-ingest`.

## What it produces

- One top-level plan row (`status='draft'`) with a filesystem-safe slug.
- A workbench directory at `$PLANAR_WORKBENCH_ROOT/<assoc-slug>/p<id>-<slug>/`.
- `product-spec.md` — product intent, user stories, non-goals, acceptance signal (registered as `kind=product_spec`).
- `tech-spec.md` — architecture, components, schema changes, and a `## Decisions` section (registered as `kind=tech_spec`).
- `roadmap.md` — flat milestone list with bulleted work items; cross-repo items carry `[touches: acme/protos, acme/service]` annotations (registered as `kind=roadmap`).
- `test-spec.md` — test strategy with scenarios authored as **flat `### Scenario: <title>` H3** (one H3 per scenario; name the coverage lens in the title, e.g. `### Scenario: Happy path — foo returns bar`), a coverage-gap checklist, and a test-surface-allocation section (unit / integration / scenario rows). The four return-path buckets (happy / empty-null / error / edge) are a **coverage-reasoning lens**, not document structure — do not use `### <bucket>` group headers with nested `#### Scenario:` H4 items. Registered as `kind=test_spec`. Frontmatter carries `verifies: [artifact:<product-spec-id>]` so the cross-reference machinery tracks which user stories the test plan covers.
- Optional initial test scenarios under `scenarios/`.
- Workbench manifest seeded via `planar workbench push`.

## What it does not do

- Does **not** create tasks. Use `pl-spec-ingest <plan>` after reviewing the drafts.
- Does **not** contact external systems (Jira, GitHub). Use `pl-ext-propagate` for that.
- Does **not** modify scope. Invoke from inside the target repo's cwd, or pass `--scope <slug>` on `plan create` to direct entities at a specific association.

## Underlying CLI verbs

The skill composes these commands in this order. The artifact rows are created with an empty body first so the returned `<artifact-id>` is known before the file is written, which lets the file carry correct YAML front matter from the start.

> **Scope.** Reads and writes use the cwd-derived scope. Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

> **Cross-scope guard.** None of the verbs below is guarded: `plan create`,
> `artifact add`, `artifact update`, and `workbench push` do not compare the
> operator's scope with a stored entity scope, and on `artifact update`
> `--scope` reassigns the artifact's stored scope rather than authorizing a
> write. Run from inside the owning repo so the cwd-derived scope is the
> intended one. See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar scope show

planar plan create "<derived title>" --slug <slug> --status draft
# → captures <plan-id>

# Repeat for each of product-spec.md, tech-spec.md, roadmap.md, test-spec.md:
planar artifact add "<title>" --kind <kind> --plan <plan-id> --body ""
# → captures <artifact-id>
# Write <filename> to the workbench directory with canonical YAML front matter
# (see Front matter contract below) followed by the planner-generated body.
planar artifact update <artifact-id> --body @<filename>

planar workbench push <plan-id>
```

## Front matter contract

Every `.md` file written by this skill carries a YAML front matter block between `---` delimiters at the top of the file. The canonical schema is the `front_matter` struct in [`src/engine/workbench/parse.cppm`](../../src/engine/workbench/parse.cppm). Files without valid front matter are treated as malformed by `workbench pull` and are rejected during sync.

Required fields for planner-written artifact files:

```yaml
---
entity_kind: artifact
entity_id: <artifact-id>        # integer returned by `planar artifact add`
anchor_plan_id: <plan-id>       # integer returned by `planar plan create`
title: <string>                 # human-readable title of this document
status: draft
artifact_kind: <product_spec|tech_spec|roadmap|test_spec>
---
```

## Questions registration

After `planar artifact update` writes the body for each spec document, the skill
iterates the `## Open questions` section in that document and registers each item
as a first-class `questions` row in the database, then links it back to the
artifact and the plan.

**When it runs:** once per artifact, immediately after `planar artifact update
<artifact-id> --body @<filename>` completes.

**Dedup check (run before each `question add`):** before registering a
candidate question, verify no existing question with the same title is already
linked to the plan:

```
existing=$(planar question list --json \
  | jq --arg t "<candidate title>" \
    '[.[] | select(.entity_links[]? | .kind=="plan" and .id==<plan-id> and .relationship=="derives-from")] | map(select(.title == $t)) | length')
# If existing >= 1, skip the add (title match is trimmed, case-sensitive)
```

If `existing` is 0, proceed with registration. If `existing` is ≥ 1, skip
`planar question add` for this item. In the re-draft scenario (same spec title,
newer artifact), link the existing question to the new artifact instead:

```
existing_id=$(planar question list --json \
  | jq -r --arg t "<candidate title>" \
    '.[] | select(.entity_links[]? | .kind=="plan" and .id==<plan-id> and .relationship=="derives-from") | select(.title == $t) | .id')
planar question link $existing_id artifact:<artifact-id> --relationship derives-from
```

**The loop (repeat for each open-questions item in the spec body):**

```
# For each item in the spec's "## Open questions" section:
# 1. Run dedup check above — skip add if title already exists for this plan.
# 2. If not a duplicate:
q_id=$(planar question add "<short title>" --body "<expanded prose>" --json | jq -r .id)
planar question link $q_id artifact:<artifact-id> --relationship derives-from
planar question link $q_id plan:<plan-id> --relationship derives-from
```

**Error handling:** question-registration failures are non-fatal. If
`planar question add` or `planar question link` returns a non-zero exit code,
log a warning and continue to the next question. Do not abort the spec-draft
run.

**Authoring conventions for `## Open questions` items:**

- **Structured (preferred):** one `### <title>` H3 heading per question with a
  paragraph body underneath. Extraction is unambiguous. New drafts use this
  convention.
- **Bulleted prose (legacy fallback):** `- <first sentence as title>. <rest of
  body>.` The first sentence (up to the first `.`) becomes the title; the full
  bullet text becomes the body. Used only when handling specs drafted before
  this convention was established.

The structured convention is what this skill produces for new drafts. The
bulleted fallback exists so that `pl-spec-ingest` can reconcile pre-fix specs
without requiring a manual rewrite.

For the rationale, keep the spec self-contained in this project: open questions
must be represented in the workbench artifact body and reconciled into first-
class question entities rather than depending on external notes.

## Status reporting

See [`agents/planner.md` § Status reporting](../../agents/planner.md#status-reporting) for the canonical phase-transition strings (`"drafting product-spec"`, `"drafting tech-spec"`, `"drafting roadmap"`, `"drafting test-spec"`, `"ready for review"`). Emit each via `planar-agent heartbeat --claim <token> --status "<text>"`; cap is 256 bytes. See [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the `awaiting:` prefix convention.

## Context

Report the resolved scope, goal, derived plan target, workbench root, and draft
mode. Distinguish a new draft from a resumed draft before creating anything.

## Intent

State in one sentence the feature goal interpreted into the four-document
planning set and its draft anchor plan.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for the plan,
four artifacts, workbench files, question registrations, links, and final
workbench push. Name every failed artifact or question target. A deduplicated
question is skipped, not applied.

## Result

Always report `outcome=ok|partial|error`. After each successful mutation, read
the persisted row with `planar plan show <plan-id> --json`,
`planar artifact show <artifact-id> --json`, or `planar question show
<question-id> --json`; after the push, inspect the workbench paths. Return the
plan ID, all verified artifact IDs and paths, registered question IDs, and the
strict-preview coverage result. Exit code zero or a written file alone is not
proof that its artifact body or links persisted.

## Warnings

Name partial artifact or question failures, a failed workbench push, strict
preview findings, unavailable post-state reads, and consequential scope or
goal assumptions. Preserve successfully registered independent rows and files;
do not claim the whole authoring sequence is atomic. An idempotent resume that
finds all four drafts current reports zero applied without a warning.

## Next actions

Give zero to three executable recommendations. For a complete draft, lead with
`planar spec ingest <plan-id> --strict --json` for review evidence and then the
operator-reviewed `planar spec ingest <plan-id> --apply` gate when appropriate.

## Recovery

For every failed target, give an exact inspect and idempotent retry command:
`planar plan show <plan-id> --json`, `planar artifact show <artifact-id>
--json`, `planar artifact update <artifact-id> --body @<path>`, or
`planar workbench push <plan-id>` as applicable. Resume against the captured
IDs and dedup checks; never recreate completed artifacts or claim rollback of
the independent plan, artifact, question, link, and filesystem writes.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
