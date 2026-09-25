# Command lifecycle integration tests

This directory holds scenarios that pass **multiple commands** through their
real CLI trees and handlers, then inspect the resulting database state. It
complements the transition matrix tests beside `src/engine/planning/` and the
single-command tests beside each handler. The complete state and edge catalog
is in [`docs/lifecycles.md`](../../../docs/lifecycles.md).

## Database fixtures

For `planar`-only scenarios, inject one shared command database object backed
by SQLite `:memory:`. Each invocation gets a fresh context and output streams,
but the database object stays alive for the whole scenario. A fresh database
object per invocation would create a fresh, empty database. Keep environment
lookup and cwd explicit so tests cannot reach the operator's Planar home.

Cross-binary and cross-process scenarios use a unique scratch database file:
SQLite `:memory:` belongs to one connection and cannot exercise the separate
connections, access policies, file locks, or startup behavior of
`planar-agent`, `planar-watch`, and `planar-ext`. The existing
[`cross_process.t.cpp`](../planar/cross_process.t.cpp) is the model for those
cases. Never simulate a second binary by giving it the operator database
object.

## Scenario inventory

Each row identifies a *workflow* to exercise through commands. The engine
tests cover the full legal/illegal transition matrices; these scenarios check
composition, persistence, audit effects, and refused writes. Add a scenario
test here when an interaction across verbs is missing, and link it in this
table. Keep narrow single-verb cases next to their handler.

| State family | Command-level scenario | Coverage |
| --- | --- | --- |
| Plan | Draft → active → paused → active → done; refuse skipped and terminal transitions | `planning_lifecycle.t.cpp` |
| Plan step | Add → in progress → done or skip; enforce step ordering | [`plan_task_remainder_leaves.t.cpp`](../planar/plan_task_remainder_leaves.t.cpp), engine transition tests |
| Task and plan roll-up | Todo → doing → done → reopen promotes, closes and reopens a child plan; dependency block/unblock | `planning_lifecycle.t.cpp`, [`task.t.cpp`](../../engine/planning/task.t.cpp), [`plan.t.cpp`](../../engine/planning/plan.t.cpp) |
| Question | Open → answered or wontfix; repeated answer and terminal guard | [`question.t.cpp`](../../engine/planning/question.t.cpp), handler tests |
| Decision | Proposed → accepted → superseded or withdrawn | [`decision.t.cpp`](../../engine/planning/decision.t.cpp), handler tests |
| Test scenario | Draft → ready → verified/failing → retired; verification audit | [`scenario.t.cpp`](../../engine/planning/scenario.t.cpp), handler tests |
| Artifact and annotation | Draft/active and active/resolved paths; terminal outcomes | [`artifact_leaves.t.cpp`](../planar/handlers/artifact/artifact_leaves.t.cpp), [`annotation.t.cpp`](../../engine/planning/annotation.t.cpp) |
| Handoff, session, context record | Capture → validate → consume; abandon and supersede paths | [`handoff.t.cpp`](../../engine/runtime/handoff.t.cpp), [`capture.t.cpp`](../../engine/runtime/capture.t.cpp), handler tests |
| Agent claim, action, workflow run | Claim → heartbeat → terminal verb; atomic task update and expiry | [`claims.t.cpp`](../planar-agent/claims.t.cpp), [`cross_process.t.cpp`](../planar/cross_process.t.cpp) |
| Routing experiment, preview, snapshot | Declare → preview → confirm → outcome; expired or consumed token refusal | [`routing` tests](../../engine/routing/), agent handler tests |
| Feedback triage | Preview → apply disposition; reproduce/report outcome | [`feedback_triage.t.cpp`](../../engine/planning/feedback_triage.t.cpp), handler tests |
| External link and sync event | Link → pull/push proposal → resolve; enforce external write boundary | [`sync.t.cpp`](../../engine/external/sync.t.cpp), [`capability.t.cpp`](../planar-ext/capability.t.cpp) |
| Workbench sync | DB ↔ files, conflict, malformed input, and no-op | [`sync.t.cpp`](../../engine/workbench/sync.t.cpp), handler tests |
| Measurement run | Start → record → finish; caller-defined terminal result | [`lifecycle.t.cpp`](../../engine/runs/lifecycle.t.cpp), handler tests |
| Derived health, closeout, resume | Inputs change → read model changes; closeout gate refusal and success | [`health` tests](../../engine/health/), [`closeout.t.cpp`](../../engine/planning/closeout.t.cpp), handler tests |

The table is an inventory, not a claim that each row already has a complete
multi-command test in this directory. Use it to choose the next scenario and
to avoid duplicating the detailed state matrix tests.
