---
slug: pl-spec-ingest
description: "Decompose workbench planning documents into a structured task graph in the Planar database."
source: agents/ingestor.md
model_tier: large
vendor:
  claude:
    argument_hint: "<plan> [--apply] [--apply-removals] [--format text|json]"
    invocation_examples: |
      /pl-spec-ingest checkout-rewrite
      /pl-spec-ingest 42 --apply
      /pl-spec-ingest checkout-rewrite --apply --apply-removals
      /pl-spec-ingest 42 --format json
shared_notes:
  - "Workbench artifacts are read through the Planar workbench contract; ingestion preview is never applied silently."
---

# Spec Ingest ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `ingestor` agent. See [`agents/ingestor.md`](../../agents/ingestor.md) for the full role spec, input/output contract, and idempotency contract.

## When to use

Invoke after the planner has drafted `tech-spec.md` and `roadmap.md` and the user has reviewed and edited them. The ingestor reads those files from the workbench and decomposes them into child plans, tasks, decisions, and test scenarios in the database.

Do **not** invoke this skill before the user has reviewed the planning documents. Task decomposition is irreversible without `--apply-removals`; review first.

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

## Question reconciliation

Before running task decomposition, the skill reconciles the `## Open questions`
section of each spec body in the plan's workbench directory against the existing
`questions` entities already linked to the plan.

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. The active scope stack was removed in plan 153 M5; there is no `scope use` to push.

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
| `--strict` | Refuse to apply when any body-only question would be silently registered. |
| `--format text\|json` | Output shape (default `text`). `--json` is the shorthand for `--format json`. |
| `--scope <slug>` | Override the cwd-derived scope. |

> Per-question interactive prompting (`--interactive`, `--yes-all`) is a deferred enhancement on the ingest verb; current behavior is silent registration unless `--strict` refuses the run.

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

## Underlying CLI verb

> **Cross-scope guard.** This verb refuses with exit 1 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or use `--no-scope-check` for legacy
> escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar spec ingest <plan> [--apply] [--apply-removals] [--format text|json]
```

## Status reporting

See [`agents/ingestor.md` § Status reporting](../../agents/ingestor.md#status-reporting) for the canonical phase-transition strings (`"reading workbench specs"`, `"decomposing tasks"`, `"writing preview"`, `"awaiting:operator-confirmation"`, `"applying"`). Emit each via `planar-agent heartbeat --claim <token> --status "<text>"`; cap is 256 bytes. The `awaiting:operator-confirmation` string uses the `awaiting:` prefix because the ingestor is genuinely blocked waiting for the explicit user gate before `--apply` may run. See [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the full convention.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
