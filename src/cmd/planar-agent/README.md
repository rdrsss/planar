# `planar-agent`

The agent-callable coordination binary. `main.cppm` registers the root
`CLI::App`; `main.cpp` supplies invocation context and dispatch. Each
`handlers/<name>/` directory defines that command's CLI shape and execution.
Its claim terminal verbs update the claim and task in one transaction.

| Handler directory | CLI app and role |
| --- | --- |
| `version/`, `schema/` | Version metadata and flat command catalog. |
| `pull/`, `peek/` | Claim the next task in one plan, or preview the same selection without a write. |
| `claim/`, `heartbeat/`, `claim_associate/` | Direct claim, lease renewal and association of an active claim with a workflow run. |
| `complete/`, `fail/`, `release/`, `block/` | Atomic claim terminals paired with task status changes. |
| `action/` | Start and end nested agent actions. |
| `run/`, `context/` | Workflow run lifecycle and context records/capsules. |
| `dispatch/` | Preview and confirm routing authorization. |
| `ingest/` | Translate vendor hook events into coordination records. |
| `queue/` | The host-wide build and test queue (plan 1080): `queue run -- <command>` waits for its turn in the agent database, runs the command, and exits with its status. |
| `reconcile/`, `abort/` | Recover expired claims, orphaned activity or a stuck claim. |
| `shared/` | CLI and handler helpers; no root CLI app. |

State transitions and the claim ritual are indexed in
[`docs/lifecycles.md`](../../../docs/lifecycles.md#3-agent-coordination-planar-agent).
