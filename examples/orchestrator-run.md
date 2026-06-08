# Example: Launch The Orchestrator

Use this after specs have been reviewed and ingested, and the anchor plan is
active.

## 1. Confirm The Plan Is Active

```text
planar plan show <plan-id> --json
planar plan update <plan-id> --status active
```

The update command is only needed if the plan is still `draft` or `paused`.

## 2. Inspect Available Work

```text
planar plan next <plan-id>
planar plan recommend-strategy <plan-id>
```

Use `plan next` to see available, claimed, stale, and blocked work. Use
`recommend-strategy` to preview whether the plan is a better fit for classic,
barrel, isolated, or fan-out execution.

## 3. Launch The Orchestrator

```text
/orchestrator <plan-id>
```

The orchestrator should:

- Read claim-aware task state.
- Surface stale claims before dispatch.
- Propose an execution strategy and dispatch shape.
- Wait for operator confirmation.
- Acquire claims through `planar-agent`.
- Dispatch coder and reviewer cycles.
- Run the test-coder pass when uncovered test-spec slugs intersect the cycle.

## 4. Preselect A Strategy When Needed

Use explicit flags only when the operator has already made the dispatch choice:

```text
/orchestrator <plan-id> --strategy classic
/orchestrator <plan-id> --strategy barrel-deferred
/orchestrator <plan-id> --strategy barrel-bypass
```

Use `barrel-bypass` only for work where gates are the whole signal, such as
mechanical documentation or generated-surface cleanup. Schema, CLI, persistence,
or cross-surface behavior changes should keep reviewer signal.

## 5. Watch Execution

In another terminal:

```text
planar-watch claims --plan <plan-id> --json --follow
planar-watch feed --plan <plan-id> --follow
```

If an agent dies or a claim becomes stale, reconcile before relaunching work:

```text
planar-agent reconcile --dry-run
planar-agent reconcile
```

