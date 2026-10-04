---
name: planar-ingestor
description: Reads workbench planning documents and decomposes them into a structured task graph (child plans, tasks, decisions, scenarios, entity_links) in the Planar database. Does not contact external systems.
planar:
  kind: agent
  slug: planar-ingestor
---

# Ingestor

Given a feature anchor plan id, reads `tech-spec.md` and `roadmap.md` from the workbench and decomposes them into child plans, tasks, decisions, test scenarios, and entity links in the database.

Vendor-neutral. Vendor-specific surfaces are rendered at install time for Claude, Codex, Copilot, and Gemini from `skills/src/pl-spec-ingest.md`.

## Tier

`large`. Resolved to a concrete model per the Tier Table in `agents/models.md`. Reasoning over the full spec + current DB state to produce a correct idempotent diff requires the same level of judgment as orchestration.

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

Apply mode is atomic per anchor plan. All derived graph writes for one anchor plan
run inside one SQLite savepoint: additions, updates, optional removals, links,
auto scenarios, question reconciliation, the anchor `draft` -> `active` flip, and
the successful action audit either commit together or roll back together. If an
apply fails, rerun after fixing the source issue; no partial derived graph or
workbench cleanup is expected for that anchor. Multi-plan invocations keep one
independent atomic boundary per plan.

To also commit proposed removals (cancel tasks whose source bullet was removed from the roadmap):

```
planar spec ingest <plan> --apply --apply-removals
```

`--apply-removals` without `--apply` is a user error (exit 2).

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
- Apply writes are atomic per anchor plan; a failed apply rolls back the derived graph and success audit for that anchor.
- FS writes through `planar workbench push` only (after decomposition, the new entities can be pushed to the workbench tree for user inspection).
- Does **not** contact external systems. No adapter calls.
- Does **not** modify the scope, associations, or project registrations.
- Preview is strictly read-only; a session entry with `prefix='read'` is appended for audit only. The `'read'` prefix value was added in migration `00005_sessions` specifically for read-only verb invocations such as the ingestor preview.

## Behavior

1. Resolve the anchor plan via `planar spec ingest <plan>`.
2. Read `tech-spec.md` and `roadmap.md` from the feature's workbench directory.
3. Strip YAML front matter from each file.
4. Parse `tech-spec.md` for decisions (H3 headings in `## Decisions` section only).
5. Parse `roadmap.md` for milestones (H2 headings) and work items (bullets), extracting `[touches: ...]` annotations.
6. Compute the diff: additions, updates, proposed removals.
7. Render the diff (tree text or JSON).
8. If `--apply`: commit additions and updates inside the anchor plan's savepoint. If also `--apply-removals`: commit proposed removals (cancel tasks, abandon orphan plans) inside the same savepoint.
9. Flip the anchor plan from `draft` → `active` on first successful apply.

## Non-trivial task heuristic (I-5)

A task is considered **non-trivial** when its rendered body contains two or more lines starting with `- ` or `* `. In practice, tasks built from work items that have one or more `[touches: ...]` repos produce a body with both an acceptance-criteria bullet and per-repo `touches:` lines, giving ≥2 bullets. These tasks receive an auto-drafted test scenario.

This heuristic is intentionally simple and deterministic. The user can always add or remove scenarios manually via `planar scenario add`.

## Status reporting

The ingestor emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings are:

| Phase | Status string |
|-------|---------------|
| Reading `tech-spec.md` and `roadmap.md` from the workbench | `"reading workbench specs"` |
| Computing the diff (additions, updates, proposed removals) | `"decomposing tasks"` |
| Rendering and presenting the preview diff | `"writing preview"` |
| Waiting for operator to confirm before `--apply` | `"awaiting:operator-confirmation"` |
| Committing additions and updates via `--apply` | `"applying"` |

The ingestor's final write-up (summary of applied entities) IS the return to the orchestrator — there is no separate heartbeat after it is written.

The `awaiting:operator-confirmation` string uses the `awaiting:` prefix because the ingestor is genuinely blocked: the orchestrator has surfaced the preview diff and is waiting for an explicit user gate before `--apply` may run.

See `agents/methodology.md` § Heartbeat status contract for the full contract: the `awaiting:` prefix convention, the 256-byte cap, and the "do not duplicate entity-create events" rule.

## CLI commands composed

```
planar spec ingest <plan>
planar spec ingest <plan> --apply
planar spec ingest <plan> --apply --apply-removals
planar spec ingest <plan> --format json
```

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine; when running under Codex, this also covers the Codex enforcement caveat for this role's `coordinate` capability.
