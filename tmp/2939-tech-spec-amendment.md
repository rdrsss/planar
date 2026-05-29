# Tech-spec amendment — artifact 142, plan 297

Operator follow-up: apply this amendment to artifact 142 (worktree-management tech-spec) from the main checkout.

## Why

The tech-spec § "Orchestration strategies" and § "Persistence model" previously claimed **no schema delta** is required because the orchestrator can reuse the existing `agent_actions.metadata` JSON column. The column did not actually exist at that time. Migration `00016_agent_actions_metadata.up.sql` lands the column. This amendment updates the spec to acknowledge the delta honestly.

## Apply from main

```sh
cd ~/projects/github/rdrsss/planar
# Edit the workbench file to apply the edits below, then push back to the DB:
planar artifact update 142 --body "$(cat /Users/mn/.planar/workbench/project_planar/p297-worktree-management/142-worktree-management-tech-spec.md)"
```

## Edits

### § Orchestration strategies (around line 72)

REPLACE:

> Five named strategies bundle the underlying dispatch axes into operator-facing choices. Storage decision: **no schema delta.** The orchestrator derives the "last-used strategy" for a plan from the most recent `agent_actions` dispatch entry's metadata JSON column; new plans default to `classic`.

WITH:

> Five named strategies bundle the underlying dispatch axes into operator-facing choices. Storage decision: **schema delta is migration 00016** (`agent_actions.metadata` nullable text column; see § Persistence model below). The orchestrator derives the "last-used strategy" for a plan from the most recent `agent_actions` dispatch entry's metadata JSON, parsed at the orchestrator-skill layer; new plans default to `classic`.

### § Persistence model (around line 146)

REPLACE:

> No new schema. The strategy choice for a given cycle lives in the existing `agent_actions` metadata JSON column on the dispatch row:

WITH:

> One new schema migration: `00016_agent_actions_metadata` adds a nullable `agent_actions.metadata` TEXT column. The engine and CLI treat it as opaque caller-attached text; the CLI validates the input as well-formed JSON at parse time when `--metadata` is supplied. The strategy choice for a given cycle lives in this column on the dispatch row:

REPLACE the trailing paragraph after the JSON block:

> The orchestrator's "last-used strategy for this plan" lookup is a query against the most recent dispatch entry's metadata. Cost: one indexed read per gate evaluation. Acceptable.

WITH:

> The orchestrator's "last-used strategy for this plan" lookup is a query against the most recent dispatch entry's metadata via `planar-watch actions --plan <id> --json` (or `--task <id>` for hand-picked dispatch). Cost: one read per gate evaluation against the existing `ix_agent_actions_kind` / `ix_agent_actions_entity` indexes — no new index required because the orchestrator already filters by entity. Write surfaces: `planar-agent pull --metadata '...'` (plan-pull dispatch) and `planar-agent action start --claim <token> --metadata '...'` (hand-picked dispatch). Read surfaces: `planar-watch actions | log | feed` include `metadata` in the `ActionRow` JSON shape as a nullable string.
