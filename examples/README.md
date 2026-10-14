# Planar Examples

Copy-paste oriented examples for common Planar flows. These are intentionally
shorter than `docs/workflows.md`: use this directory when you want to see the
shape of a session, then follow the reference docs when you need every flag and
edge case.

Replace placeholder values such as `<plan-id>`, `<task-id>` and
`<system-slug>` with values from your local Planar database.

## Examples

| Example | Use when |
|---------|----------|
| [`spec-lifecycle.md`](spec-lifecycle.md) | You want to draft specs, review them adversarially, ingest them, and prepare execution. |
| [`spec-review-loop.md`](spec-review-loop.md) | You have draft specs with open questions and want to turn them into complete, self-consistent artifacts. |
| [`operational-plane.md`](operational-plane.md) | You want to preview and propagate a finished local plan to Jira or GitHub Issues. |

## Example role agents

[`agents/`](agents/) holds example role agents, one `planar-<role>.md` file per
role: coder, reviewer, janitor, research, planner, spec-reviewer, ingestor,
ext-sync and sync-reconciler. Each is one way to divide work around Planar's
mechanism; none is required by it.

The installer does not place them. Planar installs only the `planar` skill,
and an upgrade removes the `planar-<role>` agents an older install placed. To
use one, copy it into your harness's agent directory (for example
`~/.claude/agents/`) and adapt it to your workflow. Each file carries the
frontmatter contract described in
[skill-reference.md § Frontmatter contract](../docs/skill-reference.md#frontmatter-contract).
They still cite the doctrine documents under the repository's
[`agents/`](../agents/) directory by name.

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

