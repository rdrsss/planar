# Example: Draft, Review, And Ingest Specs

Use this flow when you are starting from a feature idea and want a reviewed task
graph ready for implementation.

## 1. Draft The Planning Artifacts

```text
/pl-spec-draft "add billing export to CSV"
```

Expected result:

- A draft anchor plan.
- Four workbench artifacts: `product-spec.md`, `tech-spec.md`,
  `roadmap.md`, and `test-spec.md`.
- Open questions from the specs registered as first-class question rows.

Capture the returned plan id as `<plan-id>`.

## 2. Review The Specs Adversarially

```text
/pl-spec-review <plan-id>
```

The review should answer:

- What are we building?
- Does that match the user's intent?
- Which open questions block implementation?
- Which product, tech, roadmap, or test gaps remain?
- Are test scenarios linked to roadmap slugs?

If the verdict is `needs-answers`, answer the operator prompts before
ingestion. If the verdict is `needs-spec-work`, approve concrete edits and run:

```text
/pl-spec-review <plan-id> --write
```

`--write` applies only operator-approved artifact and question updates. It does
not ingest the specs.

## 3. Preview Ingestion

```text
/pl-spec-ingest <plan-id> --strict
```

Read the preview as the proposed task graph. Check that:

- Child plans match roadmap milestones.
- Tasks are concrete and implementable.
- Decisions are extracted from the tech spec.
- Questions are linked and no duplicate question rows are being created.
- Strict coverage does not report uncovered roadmap slugs.

If the preview is wrong, edit the workbench specs or run the spec review loop
again. Do not apply an ingest you do not understand.

## 4. Apply Ingestion

```text
/pl-spec-ingest <plan-id> --apply --strict
```

This writes child plans, tasks, decisions, questions, and scenarios to the
database.

## 5. Activate The Plan

```text
planar plan update <plan-id> --status active
planar plan next <plan-id> --json
```

The plan is now ready for orchestration.

## 6. Launch Execution

```text
/orchestrator <plan-id>
```

The orchestrator reads the active task graph, proposes a strategy, waits for
operator confirmation, and dispatches coder/reviewer cycles.

