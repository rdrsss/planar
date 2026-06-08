# Planar Workflow Templates

This directory contains **TEMPLATE** `.lua` workflows for `planar-execute`. They
are NOT auto-installed: operators copy or reference them to build their own
execution flows.

## What these are

Each file is a Lua 5.4 module consumable by `planar-execute`. It exports a table
with two fields:

- `meta` — static metadata (`name`, `description`, `phases`, `reviewer`)
- `run(ctx)` — the entrypoint; receives the `ctx` host-function surface

Templates contain `<OPERATOR-FILLS-IN>` placeholders wherever brief-composition
logic is workflow-specific. Replace these before running the template live.

## How to use

Run under `--mock-worker` first to exercise the control flow without spawning
real agent workers (no API cost):

```
planar-execute run --mock-worker --plan <plan_id> workflows/quality-spine.lua <plan_id>
```

Run live against a real plan (requires `PLANAR_EXECUTE_LIVE_AGENT=1`):

```
PLANAR_EXECUTE_LIVE_AGENT=1 planar-execute run --plan <plan_id> \
  workflows/quality-spine.lua <plan_id>
```

Inspect the workflow without running it:

```
planar-execute run --dry-run workflows/quality-spine.lua
```

## Included templates

### `quality-spine.lua` — doctrine-compliant cycle driver (canonical)

Implements the per-cycle coder → reviewer cadence from
`agents/methodology.md`. For each parallel-eligible task returned by
`ctx.eligible(plan_id)`, it dispatches a coder agent and, on success,
a reviewer agent. Sets `meta.reviewer = true` to satisfy the bright-line
refusal guard.

**Use this as the base for any plan that touches migrations, new CLI verbs,
or invariant/methodology code.**

### `parallel-fanout.lua` — parallel N-way fanout

Uses `ctx.eligible(plan_id)` + `ctx.parallel({thunks})` to fan out coders
across all parallel-eligible tasks simultaneously. Each branch spawns its
own coder + reviewer pair. Falls back with a log message when fewer than 2
eligible tasks are present (`fan_out_available = false`).

**Use this when the plan has multiple independent tasks and you want to
run them concurrently.**

## `meta.reviewer` and the bright-line refusal guard

Every workflow that may touch risky surfaces (migrations, new top-level CLI
verbs, or invariant/methodology code) MUST set `meta.reviewer = true` in its
`meta` table. Without it, `planar-execute` refuses to run the workflow against
a plan containing such tasks:

```
planar-execute: REFUSING TO RUN — bright-line refusal guard tripped …
```

`meta.reviewer = true` is a **trust-based declaration** by the workflow author
asserting that a reviewer is dispatched for every cycle. The templates include
this declaration; do not remove it unless you consciously accept the doctrine
risk (in which case pass `--bypass-reviewer-guard`).

## Deferred surfaces (not yet available)

Phase 3.5 (test-coder gated on coverage) and Phase 6 (documenter) require host
functions the harness does not yet expose:

| Phase | Host function | Blocks on |
|-------|---------------|-----------|
| 3.5 — test-coder | `ctx.test_spec_status(plan_id)` | `planar test-spec status` not on constrained worker PATH |
| 6 — documenter   | `ctx.documenter()`             | `planar-doc diff` not on constrained worker PATH (same constraint) |

The `quality-spine.lua` template has commented-out stubs for both phases.
When the missing host functions land, extend the template by uncommenting
those stubs.

## Operator extension checklist

When copying a template for a real plan:

1. Replace every `<OPERATOR-FILLS-IN>` placeholder with actual brief-composition
   logic (read task body via pre-composed args, spec citations, etc.).
2. Supply `worktree_path` and `claim_token` per-task (pass them via `ctx.args`
   after the plan_id or hard-code them in your copy).
3. Keep `meta.reviewer = true` if ANY task in the plan touches a risky surface.
4. Test under `--mock-worker` before running live.
5. Keep the extended copy out of the repo unless it is a canonical workflow
   (the templates in this directory are repo-canonical; operator extensions
   belong in `~/.planar/local/` per the local-skills boundary in
   `docs/skill-reference.md § Personal sandbox`).
