---
name: ingestor
description: Reads workbench planning documents and decomposes them into a structured task graph (child plans, tasks, decisions, scenarios, entity_links) in the Planar database. Does not contact external systems.
tier: large
role: ingestor
---

# Ingestor

Given a feature anchor plan id, reads `tech-spec.md` and `roadmap.md` from the workbench and decomposes them into child plans, tasks, decisions, test scenarios, and entity links in the database.

Vendor-neutral. Vendor-specific surfaces are under `commands/claude/pl-spec-ingest.md`, `skills/codex/pl-spec-ingest.md`, and `skills/copilot/pl-spec-ingest.md`.

## Tier

`large`. Resolved to a concrete model per [`agents/models.md`](models.md). Reasoning over the full spec + current DB state to produce a correct idempotent diff requires the same level of judgment as orchestration.

## When to use

- The planner has drafted `tech-spec.md`, `roadmap.md` (and optionally `product-spec.md`) and the user has reviewed and edited them.
- The anchor plan is in `status='draft'` and no tasks have been decomposed yet (first pass).
- The user has edited the spec and wants the task graph to reflect the edits (subsequent pass).
- The orchestrator detects that an anchor plan is in `status='draft'` and artifacts of `kind=tech_spec` and `kind=roadmap` are linked to it.

## Inputs

- A feature anchor plan id (or slug). Required.
- The workbench filesystem: reads `tech-spec.md` and `roadmap.md` from
  `$PLANAR_WORKBENCH_ROOT/<assoc-slug>/<plan-key>-<plan-slug>/`.
- The current DB state for the anchor plan (to compute the diff).

## Outputs

- **Child plans**: one per H2 milestone in `roadmap.md`, each with `parent_plan_id = <anchor>` and linked via `entity_links(relationship='derives-from')`.
- **Tasks**: one per bulleted work item, with body containing acceptance criteria, any `touches` annotation encoded as repository scope lines, and `next_action` pre-populated.
- **Decisions**: one per H3 heading in the `## Decisions` section of `tech-spec.md`, each linked via `entity_links(relationship='derives-from')` to the anchor plan.
- **Test scenarios** (draft): auto-drafted for non-trivial tasks (tasks whose body contains ≥2 bullet lines), linked via `entity_links(relationship='verifies')`.
- **Touches links**: `entity_links(relationship='touches', from=task, to=repo)` rows derived from `[touches: slug1, slug2]` annotations in roadmap bullets.
- **Anchor plan status flip**: from `draft` → `active` on the first successful `--apply` run. Idempotent on subsequent runs (status already `active`).

## Preview-first execution

The ingestor defaults to **preview mode**: no flags prints a tree-shaped diff and exits without writing. This is the safe default for orchestrators and for manual inspection.

```
planar spec ingest <plan>
```

To commit additions and updates:

```
planar spec ingest <plan> --apply
```

To also commit proposed removals (cancel tasks whose source bullet was removed from the roadmap):

```
planar spec ingest <plan> --apply --apply-removals
```

`--apply-removals` without `--apply` is a user error (exit 1).

For orchestrator / machine consumption:

```
planar spec ingest <plan> --format json
```

## Idempotency contract

Re-running the ingestor on an unchanged workbench tree is a no-op: preview reports zero operations; apply writes nothing. Reconciliation is by title under the same parent.

| Entity kind | Reconciliation key |
|-------------|-------------------|
| Child plan  | Title + parent anchor plan id |
| Task        | Title + parent child plan id (matched by title) |
| Decision    | Title + parent anchor plan id |

## Boundaries

- DB writes only through `planar` CLI verbs, and only when `--apply` is set.
- FS writes through `planar workbench push` only (after decomposition, the new entities can be pushed to the workbench tree for user inspection).
- Does **not** contact external systems. No adapter calls.
- Does **not** modify the scope, associations, or project registrations.
- Preview is strictly read-only; a session entry with `prefix='read'` is appended for audit only. The `'read'` prefix value was added in migration 0005 specifically for read-only verb invocations such as the ingestor preview.

## Behavior

1. Resolve the anchor plan via `planar spec ingest <plan>` (or the library `ingestor.Compute` function).
2. Read `tech-spec.md` and `roadmap.md` from the feature's workbench directory.
3. Strip YAML front matter from each file.
4. Parse `tech-spec.md` for decisions (H3 headings in `## Decisions` section only).
5. Parse `roadmap.md` for milestones (H2 headings) and work items (bullets), extracting `[touches: ...]` annotations.
6. Compute the diff: additions, updates, proposed removals.
7. Render the diff (tree text or JSON).
8. If `--apply`: commit additions and updates. If also `--apply-removals`: commit proposed removals (cancel tasks, abandon orphan plans).
9. Flip the anchor plan from `draft` → `active` on first successful apply.

## Non-trivial task heuristic (I-5)

A task is considered **non-trivial** when its rendered body contains two or more lines starting with `- ` or `* `. In practice, tasks built from work items that have one or more `[touches: ...]` repos produce a body with both an acceptance-criteria bullet and per-repo `touches:` lines, giving ≥2 bullets. These tasks receive an auto-drafted test scenario.

This heuristic is intentionally simple and deterministic. The user can always add or remove scenarios manually via `planar scenario add`.

## CLI commands composed

```
planar spec ingest <plan>
planar spec ingest <plan> --apply
planar spec ingest <plan> --apply --apply-removals
planar spec ingest <plan> --format json
```
