---
description: Decompose workbench planning documents into a structured task graph in the Planar database.
origin: agents/ingestor.md
shared_notes:
    - Workbench artifacts are read through the Planar workbench contract; ingestion preview is never applied silently.
slug: pl-spec-ingest
vendor:
    claude:
        argument_hint: <plan> [--apply] [--apply-removals] [--format text|json]
        invocation_examples: |
            /pl-spec-ingest checkout-rewrite
            /pl-spec-ingest 42 --apply
            /pl-spec-ingest checkout-rewrite --apply --apply-removals
            /pl-spec-ingest 42 --format json
---

# Spec Ingest ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `ingestor` agent. See [`agents/ingestor.md`](../../agents/ingestor.md) for the full role spec, input/output contract, and idempotency contract.

## When to use

Invoke after the planner has drafted `tech-spec.md` and `roadmap.md` and the user has reviewed and edited them. The ingestor reads those files from the workbench and decomposes them into child plans, tasks, decisions, and test scenarios in the database.

Do **not** invoke this skill before the user has reviewed the planning documents. A successful `--apply` persists the decomposition; later source edits reconcile through another ingest run, and removals require `--apply --apply-removals`.

## Preview-first contract

The default invocation with no flags is **read-only**:

```
pl-spec-ingest <plan>
```

This prints a tree-shaped diff of proposed additions, updates, and removals and exits without writing anything. Re-running with the same spec produces identical output (idempotent read).

## Flags and composition

| Invocation | Effect |
|-----------|--------|
| `pl-spec-ingest <plan>` | Preview only. Prints diff, no writes. |
| `pl-spec-ingest <plan> --apply` | Commit additions and updates. List removals but do not apply them. |
| `pl-spec-ingest <plan> --apply --apply-removals` | Commit additions, updates, and proposed removals (cancel tasks whose bullet was removed). |
| `pl-spec-ingest <plan> --format json` | Preview as JSON (for orchestrator consumption). |

`--apply-removals` without `--apply` is rejected as a user error (exit 1).

Apply mode is atomic per anchor plan. One anchor plan's derived graph writes,
optional removals, anchor status flip, and successful action audit commit or
roll back together. If apply fails, fix the source issue and rerun; do not
clean up partial child plans, tasks, decisions, scenarios, or workbench files
for that failed anchor. When a command ingests multiple plans, each plan keeps
its own atomic boundary and the command exits non-zero if any one fails.

## Question reconciliation

Before running task decomposition, the skill reconciles the `## Open questions`
section of each spec body in the plan's workbench directory against the existing
`questions` entities already linked to the plan.

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

### Step 1 — Extract

For each `.md` file in `~/.planar/workbench/<assoc>/p<id>-<slug>/`, parse the
`## Open questions` section using this algorithm:

Implementations may invoke `planar workbench extract-questions <plan-ref>` to get
the canonical JSON shape rather than re-implementing the parser. The algorithm
below documents what that helper does — vendor prompts can stay thin by calling
the helper.

- If the section contains `### <title>` H3 children, treat each H3 as a question
  (title = H3 text, body = paragraph content under the H3). **Preferred.**
- If the section has only `- ...` bullet items (no H3 children), treat each bullet
  as a question: title = first sentence stripped of the leading `-` and trimmed,
  body = the full bullet text. **Legacy fallback.**
- If both H3 headings and bullets appear at the same level, prefer H3 (skip
  bullets at the same nesting level as the H3s).

### Step 2 — Reconcile

Fetch existing questions linked to the plan:

```
planar question list --json
# then filter to questions whose entity_links include derives-from plan:<plan-id>
```

Compare the extracted set (body items) against the entity set:

**Dedup safeguard:** the entity-set lookup above is the dedup guard. Title
matching is trimmed, case-sensitive. A body item whose title already appears in
the plan's linked question entities is treated as "Item in both" — `question add`
is never called, preventing duplicate rows on re-ingest. In the re-draft
scenario (same spec title, newer artifact), link the existing question to the
new artifact instead of creating a new row:

```
existing_id=$(planar question list --json \
  | jq -r --arg t "<candidate title>" \
    '.[] | select(.entity_links[]? | .kind=="plan" and .id==<plan-id> and .relationship=="derives-from") | select(.title == $t) | .id')
planar question link $existing_id artifact:<artifact-id> --relationship derives-from
```

| Condition | Action |
|-----------|--------|
| Item in body, not in entities | Register via `planar question add` + `planar question link` (see loop below). Default: silent. With `--interactive`: prompt first. |
| Item in entities, not in body | Emit warning: `WARN: question "<title>" exists in entities but not found in spec body — may be answered or removed`. No write. |
| Item in both | No action (dedup safeguard fires — skip `question add`). Optionally link existing question to the current artifact if not already linked. |

**Registration loop** (repeat for each body-only question):

```
q_id=$(planar question add "<short title>" --body "<expanded prose>" --json | jq -r .id)
planar question link $q_id artifact:<artifact-id> --relationship derives-from
planar question link $q_id plan:<plan-id> --relationship derives-from
```

**Error handling:** question-registration failures are non-fatal. If
`planar question add` or `planar question link` returns a non-zero exit code,
log a warning and continue. Do not abort the ingest run.

### Step 3 — Summary line

After reconciling each spec file, emit one summary line:

```
reconciled product-spec.md: 4 questions (2 new, 2 unchanged, 0 stale)
```

### Flags

| Flag | Effect |
|------|--------|
| `--apply` | Persist the decomposition (without this flag, the run is a dry preview). |
| `--apply-removals` | Apply removals for tasks/decisions/questions no longer present in the spec. |
| `--strict` | Refuse when preview coverage has an uncovered task slug, orphan scenario, or task-slug collision. |
| `--format text\|json` | Output shape (default `text`). `--json` is the shorthand for `--format json`. |
| `--scope <slug>` | Override the cwd-derived scope. |

> Per-question interactive prompting (`--interactive`, `--yes-all`) is a deferred enhancement on the ingest verb; current behavior is silent registration. The `--strict` flag governs coverage readiness, not question registration.

### Coverage oracle by lifecycle phase

Before `--apply`, run `planar spec ingest <plan> --strict --json`. Preview is
the default, so this reads the workbench drafts without creating task or
scenario rows. Its `coverage` object is the authoritative pre-ingest oracle:
`coverage.uncovered_task_slugs` and `coverage.orphan_scenarios` must be empty,
as must the top-level `slug_collisions` array. A non-zero exit or any finding in
those arrays blocks apply.

After ingestion has been applied, use `planar test-spec status <plan> --json`
as the authoritative oracle over live task, scenario, and verifies rows. Do not
interpret zero totals from that live-row command before apply as complete draft
coverage.

## What it produces

- Child plans: one per H2 milestone in `roadmap.md`.
- Tasks: one per bulleted work item, body with acceptance criteria.
- Decisions: one per H3 heading in `## Decisions` of `tech-spec.md`.
- Questions: one per H3 heading in `## Open Questions` of `tech-spec.md`. When an H3's body starts with a `Resolution: <answer>` marker (case-sensitive `Resolution:` as the first non-blank token; single-line or multi-line), the ingestor additionally creates a `decisions` row carrying that resolution AND flips the question to `answered` with the resolution text as the answer.
- Auto-drafted test scenarios for non-trivial tasks (tasks whose body has ≥2 bullet lines).
- Touches links: `entity_links(relationship='touches')` for work items with `[touches: ...]` annotations.
- Anchor plan status flipped from `draft` → `active` on first apply.

## What it does not do

- Does **not** contact external systems (Jira, GitHub). Use `pl-ext-propagate` for that.
- Does **not** modify the workbench filesystem. Entities are written to the DB; use `planar workbench push <plan>` afterward to refresh the FS tree.
- Does **not** delete tasks without `--apply-removals`.
- Does **not** leave partial derived rows behind for a failed `--apply`; the apply boundary rolls back per anchor plan.

## Underlying CLI verb

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar spec ingest <plan> [--apply] [--apply-removals] [--format text|json] [--strict] [--json]
```

## Status reporting

See [`agents/ingestor.md` § Status reporting](../../agents/ingestor.md#status-reporting) for the canonical phase-transition strings (`"reading workbench specs"`, `"decomposing tasks"`, `"writing preview"`, `"awaiting:operator-confirmation"`, `"applying"`). Emit each via `planar-agent heartbeat --claim <token> --status "<text>"`; cap is 256 bytes. The `awaiting:operator-confirmation` string uses the `awaiting:` prefix because the ingestor is genuinely blocked waiting for the explicit user gate before `--apply` may run. See [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the full convention.

## Context

Report the resolved scope, anchor plan, workbench artifact paths, preview or
apply mode, strictness, removal policy, and output format.

## Intent

State in one sentence which reviewed draft graph will be previewed or
persisted and whether removals are authorized.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for each anchor
plan and its child plans, tasks, decisions, questions, scenarios, links, and
removals. Preserve additions, updates, unchanged, and proposed-removals detail.
Name every failed anchor or non-fatal question target rather than folding it
into a single command exit.

## Result

Always report `outcome=ok|partial|error`. Preview reports the tree diff and
coverage object with zero applied. After each successful anchor apply, read
`planar plan show <plan-id> --json` and `planar test-spec status <plan-id>
--json`; report the verified anchor status, derived identifiers and counts,
and live coverage. When multiple anchors are processed, keep the per-anchor
atomic result distinct. An unchanged preview or apply is `outcome=ok` with zero
applied and an explicit reason.

## Warnings

Name uncovered task slugs, orphan scenarios, slug collisions, stale questions,
non-fatal question registration/link failures, unavailable post-state reads,
and failed anchors. A failed anchor's derived graph rolls back atomically; a
successful independent anchor remains applied.

## Next actions

Give zero to three executable recommendations. A clean preview leads to the
operator-gated `planar spec ingest <plan-id> --apply` command, adding
`--apply-removals` only when removals were explicitly approved. A successful
apply may recommend `planar workbench push <plan-id>`.

## Recovery

For a strict-preview failure, give `planar spec ingest <plan-id> --strict
--json` after correcting the named source path. For an apply failure, give
`planar plan show <plan-id> --json` to confirm the last persisted state and the
exact idempotent retry `planar spec ingest <plan-id> --apply
[--apply-removals]`. For non-fatal question failures, include the affected
question title or ID and its exact `planar question show <id> --json`, add, or
link retry. Never prescribe cleanup for a rolled-back anchor or roll back a
successfully applied independent anchor.

## Vendor Notes

See [cross-scope-writes.md](../../agents/cross-scope-writes.md) before any write outside the cwd-derived scope.
