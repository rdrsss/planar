# Example: Draft, Review, And Ingest Specs

Use this flow when you are starting from a feature idea and want a reviewed task
graph ready for implementation.

## 1. Draft The Planning Artifacts

Ask your planner agent, or write the documents yourself, to draft the planning
artifacts for the feature. For example:

```text
Draft a product spec, tech spec, roadmap and test spec for adding billing
export to CSV.
```

A typical result:

- A draft anchor plan.
- Four workbench artifacts: `product-spec.md`, `tech-spec.md`,
  `roadmap.md`, and `test-spec.md`.
- Open questions in the specs. `planar workbench extract-questions <plan-id>`
  prints them without writing.

Note the anchor plan's id as `<plan-id>`.

## 2. Review The Specs Adversarially

Ask a reviewer, human or agent, to read the specs against the user's intent. For
example:

```text
Review the specs for plan <plan-id> for completeness, open questions and test
coverage before ingestion.
```

The review should answer:

- What are we building?
- Does that match the user's intent?
- Which open questions block implementation?
- Which product, tech, roadmap, or test gaps remain?
- Are test scenarios linked to roadmap slugs?

If the verdict is `needs-answers`, answer the operator prompts before
ingestion. If the verdict is `needs-spec-work`, edit the workbench files, then
pull the edits into the database:

```text
planar workbench pull <plan-id>
```

`workbench pull` applies filesystem edits to the database. It does not ingest
the specs.

## 3. Preview Ingestion

```text
planar spec ingest <plan-id> --strict
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
planar spec ingest <plan-id> --apply --strict
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

Work through the active task graph with whatever dispatch you use. Without a
dispatcher, claim a task directly:

```text
planar-agent pull <plan-id>
```

