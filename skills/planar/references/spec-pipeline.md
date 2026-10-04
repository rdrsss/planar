# Spec pipeline

A feature moves through six stages: draft, adversarial review, strict preview,
operator-gated ingest, workbench round trips while work runs, then closeout and archive.
Each stage has a gate; do not skip one because the previous stage looked clean. Planning
writes refuse inside a git worktree (exit 8), so run this pipeline from the parent
checkout.

## The four documents

One draft anchor plan owns four artifacts:

| File | Kind | Holds | Never holds |
|------|------|-------|-------------|
| product spec | `product_spec` | What the operator sees, does and gets; user stories; non-goals; acceptance signals. | Types, schemas, file paths, libraries, tests. |
| tech spec | `tech_spec` | Architecture, components, schema delta, `## Decisions`, `## Open Questions`. | Re-litigated user stories; scenarios. |
| roadmap | `roadmap` | Independently shippable milestones with task bullets. | Re-argued architecture or coverage. |
| test spec | `test_spec` | Scenarios as observable behaviour, a coverage-gap checklist, test-surface allocation. | Test implementations. |

Write them in that order, treating each earlier document as locked. When you reach for
something that belongs to a later document, record it under `## Open questions` and move
on.

## Grammar the ingestor reads

- **Roadmap.** Each `##` heading is a milestone and becomes a child plan. Each bullet
  becomes a task. A testable bullet ends with `[slug: <slug>]`; slugs are globally unique
  across plans. Cross-repo work adds `[touches: <repo>, <repo>]`. Ingest matches an
  existing task by its exact bullet text without the annotations, so rewording a bullet
  proposes an addition plus a removal and reports a slug collision with the old task.
- **Tech spec decisions.** Only `## Decisions` is read for decisions; each `###` under it
  becomes one decision. Do not put decision text elsewhere.
- **Tech spec open questions.** Each `###` under `## Open Questions` becomes a question.
  A body whose first token is `Resolution:` also creates a decision and marks the
  question answered.
- **Test spec scenarios.** One flat `### Scenario: <title>` per scenario, never `####`
  scenarios under bucket headers. Name the coverage lens in the title (happy path,
  empty or null, error, edge); the four buckets are a reasoning lens, not structure.
  Every scenario carries a non-empty `**Verifies:** task:<slug>` naming a roadmap slug.

## Open questions convention

Every document keeps an `## Open questions` section with one `### <title>` per question
and a paragraph body. A bulleted list is a legacy fallback that ingest still accepts
(title is the first sentence). `planar workbench extract-questions <plan> --json` returns
the canonical parse. Register each question once: check
`planar question list --plan <plan-id> --json` for the same trimmed, case-sensitive title,
then `planar question add` and `planar question link` it `derives-from` the artifact and
the plan. A failed registration is a warning, not a reason to abort the draft.

## Front matter and the stored body

- The artifact body in the database is stored **without** front matter.
  `planar workbench push` renders each file with YAML front matter (`entity_kind`,
  `entity_id`, `anchor_plan_id`, `title`, `status`, `artifact_kind`) and a generated
  header above the body.
- `--body @file` reads the file's bytes verbatim; nothing is stripped. Pass it a
  body-only file. Handing it a pushed workbench file stores the front matter inside the
  body, and the next push wraps it twice, which makes ingest see no tasks.
- Two safe edit paths: edit the pushed file in place, keep its front matter, and run
  `planar workbench pull`; or write a body-only file, run
  `planar artifact update <artifact-id> --body @<file>`, then `planar workbench push`.
- Files are named `<id>-<slug>.md`. Edit those; a leftover bare name such as
  `roadmap.md` from an older layout is not what ingest reads. Delete it and push again.
- `planar workbench lint <plan>` checks front matter without syncing.

## Stage 1: draft

1. `planar scope show`; stop with a question if the cwd resolves to no registered scope.
2. `planar plan create "<title>" --slug <slug> --status draft`.
3. For each document: `planar artifact add "<title>" --kind <kind> --plan <plan-id>
   --body ""` to get the id, write the body-only file, then `planar artifact update
   <artifact-id> --body @<file>`.
4. Register open questions as above.
5. `planar workbench push <plan-id>` to render the tree.
6. Self-check with the strict preview (stage 3). The draft is not done until it is clean.

None of these verbs is cross-scope guarded, so run from the owning repo. On
`artifact update`, `--scope` moves the artifact rather than authorizing anything.

## Stage 2: adversarial review

Read-only by default. Load the plan, its artifacts, its questions and the strict preview.
A missing core artifact ends the review at `needs-spec-work`.

1. **Intent.** Write one "what are we building" paragraph; compare it with the operator's
   stated goal. Flag product goals absent from the roadmap and roadmap work serving no goal.
2. **Questions.** Classify each as blocking, non-blocking, answered-but-not-reflected or
   duplicate. Ask the operator short, answerable questions only.
3. **Gaps.** Roles, defaults, migrations, failure and recovery, observability,
   compatibility, external propagation, docs, partial success. Run four hazard lenses
   every time: resource lifecycle and cleanup; deterministic ordering and replay;
   concurrency, ownership, cancellation and races; escaping across shell, build and
   template boundaries. Mark each finding, covered, or not applicable with evidence.
4. **Roadmap readiness.** Bullets are tasks, not themes; dependencies are ordered;
   milestones review independently.
5. **Coverage.** Every acceptance signal has a scenario or an explicit N/A.

Return exactly one verdict: `ready-for-ingest`, `needs-answers`, `needs-spec-work` or
`abort-replan`, with every packet section present (`None.` when empty). Edits are applied
only after the operator approves them in the conversation; never answer a question on the
operator's behalf.

## Stage 3: strict preview

`planar spec ingest <plan-id> --strict --json` is read-only because `--apply` is absent.
Before ingest it is the only coverage oracle: it must exit 0, and
`coverage.uncovered_task_slugs`, `coverage.orphan_scenarios` and the top-level
`slug_collisions` must all be empty. `planar test-spec status <plan-id>` reads live rows,
so before ingest it legitimately shows zeros; use it only after apply.

## Stage 4: ingest, operator-gated

1. Show the operator the preview diff and wait for confirmation of that exact invocation.
2. `planar spec ingest <plan-id> --apply`. It is atomic per anchor plan: on failure
   nothing derived is left behind, so fix the source and rerun; never clean up by hand.
   The first apply flips the anchor from `draft` to `active`.
3. Removals need their own approval and `--apply --apply-removals`; `--apply-removals`
   alone is a usage error (exit 2).
4. `spec ingest --apply` is cross-scope guarded: a mismatch exits 5 with no bypass.
5. Verify with `planar plan show <plan-id> --json` and
   `planar test-spec status <plan-id> --json`, then `planar workbench push <plan-id>`.

Editing a spec artifact after ingest turns every task packet under the anchor
`ready: false`. Batch spec edits, rerun the preview (expect zero additions, updates and
removals), and ask the operator to confirm `--apply` again before dispatching more work.

## Stage 5: workbench round trips

| Verb | Direction | Use |
|------|-----------|-----|
| `planar workbench status <plan>` | read | Drift and conflicts. Run first, every time. |
| `planar workbench pull <plan>` | files to database | After editing files. |
| `planar workbench push <plan>` | database to files | After database changes. |
| `planar workbench sync <plan>` | both | A full round; conflicts surface, never merge silently. |
| `planar workbench resolve <event-id> --prefer fs\|db` | settles one conflict | Only after the operator picks the side. |

Confirm the direction before any push or pull; reversing them overwrites the newer side.
Re-run `status` afterwards and report clean or remaining drift.

## Stage 6: closeout and archive

1. Preview with `planar plan closeout <plan-id> --dry-run --json`. The hard gate needs
   every task and descendant plan terminal and no live claims; cancelled tasks do not
   block. The git evidence is advisory. Some installed builds print the root help for
   `plan closeout --help`; use `planar schema --command "plan closeout"` there.
2. An anchor plan never auto-completes. Run closeout without `--dry-run` only after the
   operator confirms.
3. Before archiving, `planar workbench status <plan>` must be clean; sync first.
4. `planar workbench archive <plan>`, again only on confirmation. It removes the files
   only; the database keeps everything. `planar workbench restore <plan>` brings them
   back.

Report results with the envelope in [feedback-contract.md](feedback-contract.md).
