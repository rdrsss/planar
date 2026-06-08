# Example: Author A `planar-execute` Workflow

Use this when a plan needs a repeatable Lua workflow rather than a one-off
model-driven orchestrator run.

## 1. Review The Plan Shape

```text
planar plan show <plan-id> --json
planar plan next <plan-id> --json
planar plan recommend-strategy <plan-id> --json
planar test-spec status <plan-id> --json
```

These reads tell the workflow planner which tasks exist, which work is blocked,
which tasks can run in parallel, and where test coverage is still missing.

## 2. Draft The Workflow

```text
/pl-execute-workflow <plan-id> --write ~/.planar/local/workflows/<workflow-name>.lua
```

The workflow planner should choose:

- topology: sequential, fan-out, quality spine, or recovery
- worker roles: coder, test-coder, reviewer, documenter
- model-tier intent for each worker
- context capsules and must-read paths
- mock cases and live-run safety posture
- reviewer guard and bypass rules

## 3. Dry-Run The Workflow

```text
planar-execute run --dry-run ~/.planar/local/workflows/<workflow-name>.lua
```

Dry-run validates Lua shape and control flow without spawning live workers.

## 4. Mock Worker Behavior

```text
planar-execute run \
  --mock-worker \
  --plan <plan-id> \
  ~/.planar/local/workflows/<workflow-name>.lua \
  <plan-id>
```

Use mock mode to verify branching, fan-out, and reviewer gates before spending
live agent time.

## 5. Live Run

```text
PLANAR_EXECUTE_LIVE_AGENT=1 planar-execute run \
  --plan <plan-id> \
  ~/.planar/local/workflows/<workflow-name>.lua \
  <plan-id>
```

Only use live mode after dry-run and mock-worker validation pass. Keep
`--bypass-reviewer-guard` off unless the operator explicitly accepts the
quality risk.

