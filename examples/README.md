# Planar Examples

Copy-paste oriented examples for common Planar flows. These are intentionally
shorter than `docs/workflows.md`: use this directory when you want to see the
shape of a session, then follow the reference docs when you need every flag and
edge case.

Replace placeholder values such as `<plan-id>`, `<task-id>`,
`<system-slug>`, and `<workflow-name>` with values from your local Planar
database.

## Examples

| Example | Use when |
|---------|----------|
| [`spec-lifecycle.md`](spec-lifecycle.md) | You want to draft specs, review them adversarially, ingest them, and prepare execution. |
| [`spec-review-loop.md`](spec-review-loop.md) | You have draft specs with open questions and want to turn them into complete, self-consistent artifacts. |
| [`orchestrator-run.md`](orchestrator-run.md) | You have an active plan and want to launch the orchestrator safely. |
| [`planar-execute-workflow.md`](planar-execute-workflow.md) | You want to author and dry-run a `planar-execute` Lua workflow. |
| [`operational-plane.md`](operational-plane.md) | You want to preview and propagate a finished local plan to Jira or GitHub Issues. |

## Minimal End-To-End Shape

```text
/pl-spec-draft "add billing export to CSV"
/pl-spec-review <plan-id>
/pl-spec-review <plan-id> --write
/pl-spec-ingest <plan-id> --strict
/pl-spec-ingest <plan-id> --apply --strict
planar plan update <plan-id> --status active
/orchestrator <plan-id>
```

The important gates are:

- Review specs before ingesting them.
- Run `pl-spec-review` before `pl-spec-ingest --apply`.
- Run ingest preview before apply.
- Activate the plan only after the task graph is coherent.
- Let the orchestrator propose execution strategy before coder dispatch.

