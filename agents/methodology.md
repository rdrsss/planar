---
description: Shared agent orchestration methodology — the flow, iteration loop, escalation rules, concurrency rules, and state capture that the orchestrator, coder, and reviewer agents collectively follow.
kind: doc
slug: methodology
---

# Methodology

This document defines the shared policy vocabulary and rationale for how
Planar's delivery agents collaborate. Each installed role surface must remain
self-contained by inlining the load-bearing rules it executes. Repository evals
check those copies for semantic parity; links to this document provide context,
not a runtime dependency.

## Roles

| Agent          | Tier   | Role |
|----------------|--------|------|
| `orchestrator` | large  | Receives a task or task list from the user; dispatches to coders (in parallel where independent); routes coder output through reviewers; enforces the iteration cap; escalates to the user on open questions or abort. Owns the work-package as a whole. Does not write code, does not perform reviews. |
| `coder`        | medium | Implements one task end-to-end. Receives reviewer feedback on `request-changes` and addresses it in the next iteration. Does not approve its own work. |
| `test-coder`   | large  | Dispatched between coder and reviewer when `planar test-spec status` reports relevant uncovered slugs. Reads the test-spec and diff; adds repository-native verification assets; surfaces first-run failures without weakening them. Never edits production behavior. |
| `reviewer`     | large  | Reviews coder (and, when present, test-coder) output. Per iteration, decides one of `approve`, `request-changes`, `open-question`, `abort`. Does not implement fixes. |

Tier-to-model resolution: [`agents/models.md`](models.md).

## Phases

The orchestrator manages the lifecycle phases below. Phases 1–2 apply only
when the anchor plan is `draft`; execution and later phases apply to active or
completed work. Full phase documentation lives in
[`agents/orchestrator.md`](orchestrator.md#phases).

| Phase | Skill | Trigger | User gate |
|-------|-------|---------|-----------|
| 1 — Planning | `pl-spec-draft` | Goal given; no anchor plan or `draft` with no artifacts | After artifacts are drafted — user reviews before Phase 1.5 |
| 1.5 — Spec review | `pl-spec-review` | Draft artifacts have received the user's initial review signal | Findings are resolved or explicitly accepted before ingestion preview |
| 2 — Ingestion | `pl-spec-ingest` | Anchor plan `draft` with reviewed, ready-for-ingest workbench artifacts present | After preview diff — user confirms before `--apply` |
| 3 — Execution | coder + (optional test-coder) + reviewer | Anchor plan `active` or `paused` with `todo`/`doing` tasks | Strategy/isolation/model/wave preview, then nested dispatch shape; Phase 3.5 is coverage-driven per cycle; reviewer loop capped at 5 |
| 3.7 — Finalization | `janitor` (spawned subagent) | Phase 3 cycles complete with approval, validation evidence, and a confirmed Git delivery profile; user requests `--finalize` or confirms interactively | Explicit per invocation; integration must be proven and `planar plan closeout --dry-run` must pass before apply |
| 4 — Propagation | `pl-ext-propagate` | User requests `--propagate` | Explicit per invocation |
| 5 — Archive | `pl-workbench-archive` | Anchor plan `done`, user requests `--archive` | Explicit per invocation |
| 6 — Documentation | Tabularium `tabularium-documenter` | Stable integrated tree and documentation preflight configured | Missing config is a verified `not-configured` skip; configured work uses Tabularium row gates |

**Key invariants:**
- The orchestrator never auto-applies ingestion. The ingestor always runs in preview mode first; `--apply` is gated on explicit user confirmation.
- The orchestrator never ingests unreviewed planning artifacts. Phase 1.5 runs
  `pl-spec-review` after the user's initial artifact review. `needs-answer` or
  `needs-revision` stops the lifecycle; operator-approved edits are applied
  through that skill's write path and the adversarial review is rerun.
- The orchestrator never auto-archives. Archive is always an explicit user action.
- The orchestrator never finalizes silently. Phase 3.7 is gated on explicit operator opt-in (`--finalize` flag or interactive confirm). Coders never close plans; the janitor is the only agent role that runs `planar plan closeout`.
- The orchestrator never picks dispatch granularity silently. Phase 3 begins with a strict/grouped/single proposal that the user must confirm (unless an explicit `--strict`/`--grouped`/`--batch` flag was supplied at invocation).
- Phase 6 never initializes documentation configuration. Missing executable or
  repository configuration records `docs_outcome: not-configured`; a configured
  clean diff records `verified-noop`.
- For an `active` or `paused` anchor plan, the orchestrator skips Phases 1,
  1.5, and 2 and enters Phase 3 directly.
- Phase 3 work selection is claim-aware. Active, unexpired agent claims make a task or child milestone unavailable to new dispatch unless the operator explicitly forces stale-claim recovery.
- Agent-authored files must be self-contained within the target project. Do not write Planar's own internal plan IDs, task IDs, milestone labels, workbench paths, or historical shorthand into project comments, docs, specs, commit messages, or generated workflows unless the operator explicitly asks for that cross-project provenance. Use project-local names, local artifact links, or generic placeholders instead.

## Plan-status invariant

Plan status is a function of task status, enforced at task-write time. This is the plan-status auto-promotion invariant; the orchestrator does NOT need to explicitly walk child plans through `draft → active → done` after a barrel cycle.

The rule fires inside the transaction of every `task.Add`, `task.Update`, `task.Done`, `task.Reopen`, `task.Cancel`, and `task.Block`, and of every `planar-agent` terminal verb (`agent.Complete`, `agent.Fail`, `agent.Release`, `agent.Block`):

| Plan status | Task aggregate | Child plan flips to |
|-------------|----------------|---------------------|
| `draft`     | any active     | `active`            |
| `draft`     | all terminal (done + cancelled) | `done` (child); `active` (anchor) |
| `active`    | all terminal   | `done` (child only) |
| `done`      | any non-terminal added/reopened | `active` |
| `paused`    | any            | no-op (operator override) |
| `abandoned` | any            | no-op (terminal failure direction) |

**Anchor exception.** Anchor plans (`parent_plan_id IS NULL`) never auto-promote to `done` — the `done` transition is a release-gate decision and stays operator-driven. Anchors DO auto-promote `draft → active` when their own tasks start progressing.

**Direct tasks only.** A plan's status depends only on its own tasks, not on its child plans' tasks. An anchor whose child plans are all done but whose own tasks are still mixed (or absent) does NOT auto-promote — that's a deliberate non-goal (release-gate stays operator-driven).

**Opt-out.** Each task verb accepts `--no-auto-promote` to skip the recompute for that single operation. Used by migrations and scripted bulk edits that don't intend the plan-level transition. For example, `--apply-removals` may cancel tasks and then explicitly abandon the parent plan; the cancellation must NOT auto-promote (cancel→done would forbid the subsequent abandon).

**Audit trail.** Every transition emits a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `plan_status: <id>`. The `session_entries.prefix` CHECK constraint does not include a dedicated `status` prefix, so the sentinel-body convention is the grep-recoverable alternative. The same sentinel-body pattern applies to dispatch audit entries.

Recover the per-transition record with:

```
planar audit trail --kind plan <plan-id> --grep "^plan_status:"
```

The full body schema:

```
plan_status: <plan id>
plan_title: <title>
from_status: <draft|active|paused|done>
to_status: <active|done>
trigger: <task_add|task_update|task_done|task_reopen|task_cancel>
trigger_task_id: <id>
task_aggregate: todo=N doing=N blocked=N done=N cancelled=N
```

Session-entry emission is best-effort: if no active session exists or the sessions table is absent (test fixtures), the helper silently no-ops the entry while still completing the status UPDATE. The invariant prefers transactional correctness over forensic completeness.

## Coordination claims

Planar is the synchronization plane for multiple vendors working from separate console windows. Task status alone is not sufficient to answer "what is next" because a task can still be `todo` while another vendor is already working on it. The live ownership contract is an agent work claim.

The claim ritual is expressed entirely in `planar-agent` verbs (the dedicated agent-side binary; there is **no** `planar agent` subcommand on the `planar` binary). The canonical sequence is **pull → heartbeat → terminate** with `complete` / `fail` / `release` / `block`:

1. **Check.** Before selecting work, the orchestrator runs `planar plan next <plan>` (operator-side claim-aware read) or `planar-agent peek <plan>` (agent-side dry-run of "what would `pull` pick"). Do not derive next work from `task list --status todo` alone.
2. **Pull.** `planar-agent pull <plan-id> [--role coder] [--worktree <id-or-path>]` atomically picks the next eligible task, claims it (`agent_work_claims.status='active'`), flips the task to `doing`, and starts a top-level `agent_actions` row. Returns `{claim_token, task, action_id}`. When no work is available it returns `{ok:true, no_work:true}` and the agent terminates cleanly.
   For dispatch where the caller already has a specific task ID (orchestrator hand-picking), use `planar-agent claim --entity task:<id> [--role <r>] [--worktree <path>]` instead. A direct task claim atomically flips `todo` to `doing`, so the ordinary terminal verbs work without an operator-side status update. Use `--no-transition` only when the caller deliberately needs the pure claim primitive; plan and plan-step claims never change entity status.

   **Cross-session dispatch hierarchy (`--parent-action`).** The orchestrator
   creates its coordination action under a plan-level claim; it never uses
   `pull --role orchestrator`, because `pull` consumes a feature task. The
   coder's task pull names the plan-coordination action as its parent:

   ```sh
   orch_claim=$(planar-agent claim --entity plan:$PLAN_ID --role orchestrator \
     --purpose "coordinate plan $PLAN_ID" --json)
   orch_token=$(echo "$orch_claim" | jq -r .claim_token)
   orch_action=$(planar-agent action start --claim "$orch_token" \
     --kind orchestrator --json | jq -r .action_id)

   # Coder dispatch: --parent-action wires the cross-session hierarchy edge:
   planar-agent pull $PLAN_ID --role coder --parent-action "$orch_action" --json
   ```

   Without `--parent-action`, the coder's action is a root (no parent); `planar-watch tree` renders it as a separate, disjoint chain with no connection to the orchestrator. Omitting the flag preserves today's behavior bit-for-bit and is the correct choice when the caller does not want tree hierarchy (e.g. bare `pull` for non-orchestrated work). The flag is validated as a positive integer; an unknown action id causes `--parent-action` to fail with `NotFound`.
3. **Heartbeat.** `planar-agent heartbeat --claim <token> [--ttl <secs>]` at least once per TTL/2 while work continues. A long-running tool call may delay the heartbeat, but the agent should heartbeat immediately before and after such calls.

   Omitting `--ttl` RENEWS the lease length the claim currently holds — heartbeating an 8h claim keeps 8h. Pass `--ttl` only to change the lease deliberately; it then sets the new length absolutely, in either direction. (Before task 6093 an omitted `--ttl` reset the lease to a fixed 600s default, so heartbeating a long claim *truncated* it to ten minutes and a faithfully-heartbeating dispatch was more likely to lose its claim than one that never heartbeated. Briefs written against that behavior repeat `--ttl` on every heartbeat; that is still correct, just no longer necessary.)
4. **Report sub-actions (optional).** For granular telemetry, wrap tool calls in `planar-agent action start --claim <token> --kind tool_call` / `planar-agent action end --action <id> --outcome ok`. Most agents skip this and let the top-level action started by `pull` cover the whole work session.
5. **Terminate** with exactly one of:
   - `planar-agent complete --claim <token> [--summary <text>]` — work succeeded; task → `done`, claim → `completed`.
   - `planar-agent fail --claim <token> --reason <text>` — work failed; task back to `todo`, claim → `aborted`.
   - `planar-agent release --claim <token> [--reason <text>]` — graceful give-up without attempting; task back to `todo`, claim → `released`.
   - `planar-agent block --claim <token> --blocker <task-id> [--reason <text>]` — hit an external blocker; task → `blocked`, blocker edge created, claim → `released`.

   If the process dies without invoking any of these, `planar-agent reconcile` (operator-side, separate binary) later marks the lease stale. The task stays in `doing` until the operator revives it or another agent's `pull` finds it available again (a stale claim no longer blocks pull's exclusivity).
6. **Operator recovery.** Stuck claims and the human cleanup path live on `planar-agent`, not `planar`: `planar-agent reconcile [--dry-run] [--stale-after <secs>]` for batch stale-claim sweeps, and `planar-agent abort --claim <token> --reason <text>` to force-release a specific claim regardless of the owning session.
7. **Live observability.** The read-only `planar-watch` binary carries `ps`, `feed --follow`, `log`, `claims`, `actions`, and `plans` for streaming human observation. For non-streaming reads through the `planar` binary, use `planar dashboard --agents`, `planar plan next`, `planar tree`, `planar audit trail`, and `planar health`.

The default claim scope is exclusive for tasks and child milestones. Shared claims are reserved for read-only observation or plan-level coordination where multiple agents are intentionally watching the same entity. Overlapping exclusive live claims are a conflict, not a scheduling hint.

Dispatch session entries include claim tokens so interrupted cycles can be reconstructed:

```
dispatch_shape: <shape>
reviewer_disposition: <dispatched|skipped-by-profile|deferred|bypassed>
cycle_scope: <plan:N milestone:M | task:T...>
tasks: [<id>, <id>, ...]
claim_tokens: [<token>, <token>, ...]
```

Claims are the synchronization gate. Reviewer dispatch and Phase 3.5 remain the quality and coverage gates.

## Finalization dispatch contract

Finalization is the post-approval phase that turns reviewer-approved, committed,
integrated work into a closed plan. It is distinct from task completion (whose
terminal transition the orchestrator owns outside the explicit in-pwd
`barrel-bypass` exception) and from archive (which removes the workbench FS
tree). The orchestrator drives finalization by dispatching the **janitor** as a
spawned subagent — the same isolation model as coder/reviewer dispatch.

### Who does what

| Agent | Allowed | Not allowed |
|-------|---------|-------------|
| Coder | Heartbeats, repository changes, structured validation evidence, and a work-complete report. Only under an explicitly selected in-pwd `barrel-bypass` cycle may it run `planar-agent complete --claim <token>`. | `planar plan closeout`, `planar task done`, or any other terminal/status transition |
| Orchestrator | Routes exactly one `planar-agent complete`, `fail`, `release`, or `block` terminal verb after the cycle disposition; dispatches janitor for plan closeout | `planar plan closeout` directly; finalizing silently |
| Janitor | `planar plan closeout <plan-id>` (after gate passes); merge, reconcile, cleanup | Bypass the `--dry-run` gate; force-close a `ready: false` plan |

**Capability boundary is hard:** coders never gain plan-mutation power. The orchestrator dispatches the janitor; the janitor is the only agent role that runs `planar plan closeout`.

### Finalization task slug convention

The janitor and orchestrator may need to create discrete task rows to track finalization work (merging, reconciling Planar state, cleaning up). These tasks MUST carry a recognizable slug prefix so they are distinguishable from feature tasks:

| Prefix | When to use |
|--------|-------------|
| `finalize-<description>` | Generic finalization steps (e.g. `finalize-migrate-schema`) |
| `merge-<description>` | Branch / PR merge steps (e.g. `merge-feature-billing-export`) |
| `reconcile-<description>` | Planar DB reconciliation steps (e.g. `reconcile-stale-claims`) |

Example:
```bash
planar task add --plan <anchor-id> --slug merge-feature-billing-export "merge feature/billing-export to main"
planar task add --plan <anchor-id> --slug reconcile-stale-claims "reconcile stale agent_work_claims after merge"
```

These tasks follow the same terminal rules as any task — they must reach `done` or `cancelled` before `planar plan closeout` can pass the hard gate. The convention is purely for human-readable labeling and for `planar plan closeout --json`'s `hard_evidence.finalization_tasks` count, which surfaces how many finalization-prefixed tasks exist as an advisory audit field. No schema change is required; the prefix is in the `tasks.slug` column which already exists.

### Dispatch sequence

1. **Precondition check.** Before dispatching the janitor, the orchestrator
   confirms an approved cycle (or explicit bypass), complete structured
   validation evidence, and an operator-confirmed Git delivery profile. The
   profile selects `github-pr`, `external-pr`, `local-ref`, or
   `already-integrated` and supplies exact verification/integration commands.
2. **Gate: explicit operator opt-in.** Finalization only runs when the operator supplies `--finalize` at invocation or confirms interactively. The orchestrator never triggers it automatically on cycle completion or plan status change.
3. **Janitor dispatch.** The orchestrator spawns a fresh janitor through the
   host's subagent surface. The brief contains the delivery profile, anchor plan
   id, owned worktrees/branches, reviewer or bypass disposition, structured
   validation evidence, and session context.
4. **Janitor executes its six-step flow.** See
   [`agents/janitor.md`](janitor.md): verify evidence → integrate and prove Git
   state → post-integration validation → reconcile Planar → clean owned Git
   state → `planar plan closeout`.
5. **Gate verdict surfaces to operator.** The orchestrator reports the janitor's result:
   - `closed` — plan is `done`, branches/worktrees clean, claims reconciled.
   - `blocked-with-reasons` — `planar plan closeout --dry-run` returned `ready: false`; the `blocked_by` list is surfaced verbatim and no closeout is applied.
6. **Heartbeat.** The orchestrator heartbeats its own claim while awaiting the janitor: `planar-agent heartbeat --claim <token> --status "awaiting:janitor"`.

### Relationship to Archive

Finalization (Phase 3.7) does merge + DB closeout: the plan transitions to `done` through the gate. Archive (Phase 5) does workbench FS archival (`planar workbench archive`). They are distinct steps. Finalization typically runs first; Archive can follow in the same invocation via `--finalize --archive`.

### External harness finalization hook

An external workflow harness may expose a **finalization hook after a clean workflow run** — the harness-side equivalent of the orchestrator's Phase 3.7. When a `.lua` workflow script completes all its worker tasks without a `failure-surfaced` or `abort` outcome, the harness is expected to invoke the janitor (or run `planar plan closeout` directly as an operator-aligned harness call) to mirror what the model-driven orchestrator does in Phase 3.7. This is an **integration point**; its contract here is:

- The hook fires only on a clean workflow run (all workers green, no unresolved test-coder failures, no orphaned claims blocking the gate).
- The hook is gated identically to the orchestrator's Phase 3.7: `planar plan closeout --dry-run` must pass before apply; `ready: false` surfaces to the operator and halts without force-closing.
- Coders dispatched by an external harness still never call `planar plan closeout`; the finalization hook is the harness-level call, operating at the operator-plane boundary, not the agent-plane boundary.
- In mock mode the hook should be a no-op or dry-run-only.

## Flow

1. **Intake.** The orchestrator receives one or more tasks (or a goal, or an anchor plan id). It determines the current phase based on the anchor plan's status and confirms scope and acceptance signal. Tasks it cannot unambiguously scope produce an `open-question` immediately and are paused.
2. **Claim-aware selection.** The orchestrator reads `planar plan next <plan>` and excludes active unexpired claims from runnable work. Stale claims are surfaced to the operator or reconciled explicitly before dispatch.
3. **Dispatch.** The orchestrator claims each runnable cycle, records claim tokens in the dispatch entry, and hands the claimed task(s) to a `coder`. Independent claimed cycles may run in parallel (see [Concurrency](#concurrency)).
4. **Implementation.** The coder heartbeats the claim while working, produces
   the change set, runs every required entry in the confirmed validation
   profile, and returns structured evidence to the orchestrator without
   changing task status.
5. **Review.** The orchestrator hands the change set and claim token(s) to a `reviewer`. The reviewer decides one of `approve`, `request-changes`, `open-question`, `abort` and treats edits outside the leased scope as scope drift.
6. **Loop or terminate.** On `request-changes`, the orchestrator returns to
   implementation with the same claim if the lease is still valid, or
   renews/reclaims explicitly. On terminal outcome, the orchestrator invokes
   exactly one atomic terminal verb. The sole exception is an explicitly
   selected in-pwd `barrel-bypass` cycle, where the coder may invoke
   `planar-agent complete` after all required validation entries pass. The loop
   is bounded by the iteration cap below.

## Durable orchestration boundary contract

A verified slice, reviewer decision, wave barrier, operator gate, or failure is
only a **candidate boundary**. It becomes durable for a target only after the
orchestrator has persisted and validated that target's recovery state. The
orchestrator MUST NOT describe a candidate boundary as clean or durable merely
because a subagent returned, a terminal verb fired, or a session is about to
yield.

### Surviving target

The unit that survives interruption is the task, identified by `task:<id>` —
not the model process, action, claim, session, branch, or worktree. After the
candidate-boundary operation and any atomic terminal verb have completed, every
task in the dispatch scope whose authoritative status is `todo`, `doing`, or
`blocked` is a surviving target. This includes a task returned to `todo` by
`planar-agent fail` or `release`, and a task moved to `blocked` by
`planar-agent block`. In a grouped cycle or wave, classify and checkpoint each
task independently; one invalid target does not erase or roll back another.

Tasks whose authoritative status is `done` or `cancelled` are terminal. They
are excluded from the surviving-target gate: the atomic terminal record is
their durable evidence, and the orchestrator MUST NOT manufacture a resume
snapshot or next action for them.

A pre-dispatch operator gate is not a candidate boundary and has no surviving
target. Before the Phase 3 gates are accepted, intake is read-only: do not
write `next_action`, sessions, snapshots, claims, actions, or other Planar
state. The checkpoint contract starts only after operator-approved work has
begun.

### Per-target checkpoint

Before yielding control, process each surviving target in this order:

1. **Write an exact next action.** Run `planar task update <task-id>
   --next-action "<exact-action>"`. The value must tell a zero-context agent one
   concrete action to perform, where to perform it (a command, file/symbol, or
   named operator gate), and the observable success condition. Include any
   prerequisite that is not recoverable from the resume packet. Phrases such as
   `continue`, `finish the task`, `implement per acceptance criteria`, or
   `resume work` are not exact next actions.
2. **Capture stage, iteration, and result.** Run `planar capture snapshot
   --task <task-id> --next-action "<same-exact-action>" --note
   "<checkpoint-body>" --json`. The snapshot body uses this line-oriented
   contract:

   ```text
   orchestration_checkpoint: v1
   stage: <verified-slice|reviewer-decision|wave-barrier|operator-gate|failure>
   iteration_scope: <coder-review|test-coder|none>
   iteration: <positive-decimal|0>
   result: <stable-outcome-token>
   ```

   `iteration` is the current iteration within `iteration_scope`; use
   `iteration_scope: none` and `iteration: 0` when no iteration applies (for
   example, a pre-dispatch operator gate or gates-only barrel cycle). `result`
   comes from the structured gate, reviewer, terminal, or wave result rather
   than an invented prose summary. The snapshot augments existing attribution;
   it does not replace the claim token, handoff, branch, worktree, commit, or
   action records already captured by their owning mechanisms.
3. **Validate.** Run `planar resume validate <task-id> --json`. Only
   `resumable:true` makes this target's boundary durable. On success, return
   this exact recovery command: `planar resume <task-id> --json`.

If validation fails, the orchestrator returns `outcome=partial` for the
boundary, leaves that target at its current authoritative status, and does not
advance or redispatch it. The result names `task:<id>`, includes every returned
`failures[].check`, `message`, and `remediation`, and ends with the exact retry
command `planar resume validate <task-id> --json`. When the orchestrator knows
the missing value (especially the next action), it substitutes that shell-safe
value into the repair command rather than surfacing a placeholder. Independent
targets that validated successfully remain durable and applied.

Between candidate boundaries, every held claim still follows the TTL/2
heartbeat rule. In particular, heartbeat all surviving held claims immediately
after each serial subagent dispatch returns; checkpointing is not a substitute
for lease renewal.

## Quota-aware staged dispatch policy

`parallel-fanout` starts capacity-unknown work in bounded waves. At the
strategy gate, the operator or vendor surface sets a maximum wave size; the
orchestrator records that choice with the dispatch preview and never starts
more than that many lanes at once. If no maximum is supplied, the orchestrator
proposes a conservative bounded size and waits for confirmation instead of
launching the full eligible set. Dependency barriers and the engine's
parallel-eligibility result remain authoritative: the capacity bound may split
an eligible wave into smaller dispatch waves, but it never makes a serialized
or blocked lane eligible.

The model-driven orchestrator maintains one circuit breaker per provider for
the current run. A terminal categorized as `usage_limit`, `context_limit`, or
`output_limit` opens only that provider's breaker on the first occurrence.
After it opens:

1. Do not start any later, not-yet-running lane assigned to that provider.
2. Let already-running independent lanes reach their ordinary terminal result;
   do not cancel them or erase landed work.
3. Continue eligible lanes assigned to unaffected providers, still respecting
   their wave-size bounds and dependency barriers.
4. Run the existing wave barrier and explicit reconcile path, then apply the
   durable-boundary checklist independently to every surviving target.

Opening a breaker never reconciles, aborts, reclaims, or otherwise mutates a
live claim automatically. A clean categorized terminal remains governed by its
atomic terminal result. A dead or abandoned claim is reported with the exact
`planar-watch claims --json` inspection and `planar-agent reconcile --dry-run`
recovery commands; an operator must explicitly choose any subsequent abort or
reconcile mutation. Successfully landed lanes and independent artifacts remain
applied, and the orchestration result claims no cross-lane rollback.

Before yielding, the result and recovery packet name the affected provider and
systemic category, the breaker as open, the landed and already-running lanes,
the unfinished provider-blocked lanes, the unaffected lanes allowed to
continue, and each surviving task's exact `planar resume <task-id> --json`
command. Circuit state is model-layer state, not a new persisted scheduler or
background process.

On a zero-context resume, re-read the wave's structured claim outcomes,
rederive each provider's breaker state, run `planar resume <task-id> --json`
for every surviving target, and recompute the unfinished eligible set. An open
breaker closes only after the operator explicitly chooses to dispatch that
provider again and confirms a new maximum wave size. Elapsed time, a fresh
session, or capacity on another provider never closes it implicitly. Provider
API polling, quota prediction, automatic retry timing, and automatic claim
reconciliation remain out of scope.

## Orchestration strategies

An orchestration strategy is a named bundle of four tunable dispatch axes:
`isolation`, `branch_model`, `concurrency`, and `reviewer_cadence`.
Test-coder coverage is not tunable by strategy: Phase 3.5 runs per coder cycle
whenever its coverage predicate is true. Strategy is the operator-facing
dispatch frame; the dispatch-shape gate runs nested under it.

Storage is path-of-least-resistance: **no schema delta.** The strategy choice for each cycle lives in the existing `agent_actions.metadata` JSON column on the dispatch row; the orchestrator derives the "last-used strategy for this plan" by reading the most recent dispatch entry's metadata. New plans default to `classic`.

### Named strategies

Four named strategies run in the **model-driven orchestrator**. The three sequential strategies (`classic`, `barrel-deferred`, `barrel-bypass`) can run in either isolation mode: `pwd` (default continuity path) or `worktree` (dedicated epic/cycle worktree). `parallel-fanout` always runs in worktrees because fan-out without isolation is invalid. An operator who wants something outside the menu can assemble a custom combination via axis-by-axis flags (see [Strategy gate](#strategy-gate) below).

- **`classic`** — Sequential cycles, reviewer per cycle, test-coder per cycle. Default isolation is `pwd` on the current branch; selectable worktree isolation runs each cycle in a `cycle/p<plan>/<task-slug>` worktree off the plan's epic branch. Recommended for: single-task changes, small plans, high-stakes invariant-touching work.
- **`barrel-deferred`** — Coder cycles run back-to-back; reviewer dispatched once at a milestone or plan boundary on the union diff. Default isolation is `pwd`; selectable worktree isolation keeps the operator checkout clean while preserving the deferred-review cadence. Recommended for: long sequential plans where per-cycle reviewer overhead exceeds the value.
- **`barrel-bypass`** — No reviewer at all; the target repository's confirmed
  validation profile is the entire signal. Default isolation is `pwd`;
  selectable worktree isolation is allowed when rollback or checkout hygiene
  matters. This is an expert, explicit opt-in for operators who accept
  gates-only risk. It is supported but never recommended by the orchestrator.
- **`parallel-fanout`** — N coders fanned out across the parallel-eligible subset, each in its own worktree off a shared epic branch, staged into dependency-respecting waves and consolidated at fan-in. **Model-runnable** (plan 760): the model orchestrator drives the worktree/branch/merge git ops and spawns the N coders concurrently through the host's subagent dispatch surface, while the deterministic wave/lane/merge/teardown computation lives in the spawn-free `workflows/parallel-dispatch.lua` seam (which reads `recommend-strategy` + the `depends-on` graph and HANDS BACK). There is **no external harness** in this path — the runner is the model orchestrator plus existing Planar primitives (`recommend-strategy`, `planar-agent pull --worktree`, `planar-agent reconcile`, the atomic terminal verbs). Recommended for: multi-lane plans (≥3 tasks, ≥2 parallel-eligible) whose lanes touch disjoint files.

Phase 3.5 remains per coder cycle under every named strategy. Strategy changes
reviewer cadence, never the coverage gate.

`isolated-sequential` is retained only as a descriptive alias for `classic` + `worktree` isolation. It is no longer an external-harness-only escape hatch. The same spawn-free seam that powers `parallel-fanout` exposes a `cycle_plan` phase for single-lane worktree bookkeeping; the model still performs the git worktree/branch/merge operations and spawns the coder.

### Continuity guarantee: `classic`

`classic` with `pwd` isolation is the explicit continuity default, not a legacy or deprecated mode. It matches today's operator behavior bit-for-bit: coder in pwd, current branch, sequential cycles, reviewer per cycle. An operator who picks (or accepts the recommendation of) `classic` + `pwd` sees no behavioral change relative to today — no worktree is created, no epic branch is cut, no parallel dispatch happens. This is a first-class supported strategy and a design promise: introducing the strategy menu must not require existing operators to learn a new flow to keep working as they do.

### Axes

The four tunable axes underlying every strategy. The model-driven orchestrator
reaches both isolation values for sequential strategies (`pwd`/`worktree`) and
the `fan-out` bundle through `workflows/parallel-dispatch.lua`.

| Axis | Values | `classic` default |
|------|--------|-------------------|
| `isolation` | `in-pwd`, `worktree` | `in-pwd` |
| `branch_model` | `current-branch`, `epic-child` | `current-branch` |
| `concurrency` | `sequential`, `fan-out` | `sequential` |
| `reviewer_cadence` | `per-cycle`, `per-fanin`, `at-boundary`, `gates-only` | `per-cycle` |

### Named bundles

Each strategy locks in concurrency and review cadence. Sequential strategies default to `in-pwd`/`current-branch` but may be confirmed with `worktree`/`epic-child` isolation. `parallel-fanout` forces `worktree`/`epic-child`.

| Strategy | `isolation` | `branch_model` | `concurrency` | `reviewer_cadence` |
|----------|-------------|----------------|---------------|--------------------|
| `classic`             | in-pwd default; worktree optional | current-branch default; epic-child with worktree | sequential | per-cycle |
| `barrel-deferred`     | in-pwd default; worktree optional | current-branch default; epic-child with worktree | sequential | at-boundary |
| `barrel-bypass`       | in-pwd default; worktree optional | current-branch default; epic-child with worktree | sequential | gates-only |
| `parallel-fanout`     | worktree | epic-child | fan-out | per-fanin |

Both worktree shapes are model-runnable via `workflows/parallel-dispatch.lua`: `cycle_plan` computes one sequential lane, while `plan`/`waves` compute fan-out lanes.

### Invalid combinations

Invalid-combination guards still hold. `fan-out` requires `worktree` + `epic-child` and a fan-in review/test cadence; `fan-out` + `in-pwd`, `fan-out` + `current-branch`, or `per-cycle` + `fan-out` is refused. Sequential `worktree` requires `epic-child`; `worktree` + `current-branch` is refused because commits would land on the operator branch from an isolated checkout. `in-pwd` + `epic-child` is refused because there is no separate checkout to hold the child branch.

### Recommendation algorithm

The orchestrator proposes a strategy per plan based on plan shape, with status-quo bias. The algorithm runs in the orchestrator skill, sourcing the inputs it needs from existing CLI reads (`planar plan show --json`, `planar task list --json`, `planar audit trail`). If the in-skill implementation drifts, an engine-side `planar plan recommend-strategy --json` flag becomes the parallel of the deferred `--parallel-eligible` flag.

1. `barrel-bypass` is excluded from recommendation. It may be selected only by
   an explicit operator choice at the strategy gate or invocation flag.
2. If the plan has a multi-milestone roadmap and at most one parallel-eligible task per milestone → recommend `barrel-deferred`.
3. Else if the plan has ≥3 tasks with ≥2 parallel-eligible → recommend `parallel-fanout` (model-runnable via the `workflows/parallel-dispatch.lua` seam — staged worktree waves, per-lane coders fanned out concurrently, fan-in merge, no external harness).
4. Else if the plan has exactly 1 task → recommend `classic`.
5. Else if the most recent dispatch on this plan used a non-default,
   non-bypass strategy `S` → recommend `S` (stickiness — the operator already
   made a choice for this plan). A prior bypass remains visible in history but
   is never re-proposed automatically.
6. Otherwise → recommend `classic` (status-quo bias).

The recommendation is a proposal, never an action. The strategy gate (below) is what turns it into a chosen strategy. Isolation is confirmed alongside the strategy: recommend `pwd` unless the operator requested worktrees, the prior cycle used worktrees, or the plan shape makes pwd hygiene materially valuable; in those cases surface `worktree` as the recommended isolation. `parallel-fanout` always sets isolation to `worktree`.

### Strategy gate

Phase 3 of the orchestrator now runs **two** gates in order before dispatch:

1. **Strategy + isolation gate** (new). "Which strategy, and should it run in pwd or worktrees?" The orchestrator surfaces its recommended strategy + recommended isolation + a one-line rationale + the named alternatives + the **dispatch preview** (the task breakdown rendered as the ordered `depends-on`-subgraph via the seam's `waves` phase — a presentation projection under every strategy, not only `parallel-fanout` — with the proposed Axis C model tier per task). The operator confirms or overrides both axes and may override any task's tier before confirming; the confirmed tier map is binding for dispatch (see [`agents/models.md` §Coder tier policy](models.md#coder-tier-policy)).
2. **Dispatch-shape gate** (existing — see [Dispatch Granularity](#dispatch-granularity)). "Within that strategy, which shape for this cycle?" Constrained by the strategy: `barrel-bypass` forces the `barrel-bypass` shape; `barrel-deferred` forces the `barrel-deferred` shape; `parallel-fanout` forces the `fan-out` shape; `classic` keeps the full strict / grouped / single menu.
3. **Dispatch.** For `pwd` isolation the orchestrator claims tasks and dispatches coders in the operator checkout. For sequential `worktree` isolation, the orchestrator drives one cycle lane at a time: call `planar-execute run workflows/parallel-dispatch.lua --phase cycle_plan`, cut/create the epic and cycle worktrees from the seam output, claim with `planar-agent pull --worktree <path>` (or `claim --entity ... --worktree <path>`), spawn the coder in that worktree, then merge the completed cycle branch into the epic worktree. For `parallel-fanout`, the orchestrator drives staged worktree fan-out itself — the seam computes the current wave / per-lane worktree paths / merge order, the model cuts the epic branch, creates the per-lane worktrees, spawns N coders concurrently (each `isolation: worktree`, each with its own `planar-agent pull --worktree` claim), and runs the fan-in merge.

The strategy gate is operator-confirmed by default. Skip flags:

- `--strategy <name>` — pre-commit to a named strategy. Skips the strategy part of the gate; isolation still defaults to `pwd` for sequential strategies unless `--isolation worktree` is supplied. The dispatch-shape gate still runs unless that gate also has a pre-committed answer.
- `--strategy custom --isolation <X> --branch-model <Y> --concurrency <Z> --reviewer-cadence <W>` — pre-commit to a custom axis combination. Phase 3.5 remains coverage-driven per cycle.

Auto-defaulting without confirmation is **not** a supported mode — the recommendation engine never silently picks a strategy. If the operator wants zero-friction repetition, `--strategy <name>` is the explicit opt-in. No flag pre-commits Axis C (model tier): under a fully-flagged invocation the dispatch preview is still printed, default-tier tasks proceed without prompting, and any proposed `large` escalation requires explicit confirmation before that dispatch.

**Persistence.** No new schema. The orchestrator writes the chosen strategy into the dispatch entry's `agent_actions.metadata` JSON column on the dispatch row:

```json
{
  "strategy": "barrel-deferred",
  "axes": {"isolation": "worktree", "branch_model": "epic-child", "concurrency": "sequential", "reviewer_cadence": "at-boundary"},
  "dispatch_shape": "grouped",
  "model_tiers": {"14": "medium", "15": "large"},
  "rationale": "multi-milestone plan, low per-cycle review value"
}
```

The "last-used strategy for this plan" lookup that drives rule 6 of the recommendation algorithm is a single indexed read against the most recent dispatch entry's metadata for the plan. No `plans.strategy` column, no separate `plan_strategies` table — operational state belongs to the dispatch row that recorded the choice.

### Relation to existing dispatch shapes

The dispatch-shape gate's six shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`) map onto the strategy axes as follows:

- `strict`, `grouped`, `single` — apply within `classic` regardless of isolation. They describe per-cycle batching, not overall methodology. Under the barrel strategies they are subsumed; under `parallel-fanout` the shape is forced to `fan-out` (each wave's lanes are batched one coder per lane).
- `barrel-grouped`, `barrel-deferred`, `barrel-bypass` — these conflate "dispatch shape" with "reviewer cadence." Under the strategy model, the latter two are subsumed by the `barrel-deferred` and `barrel-bypass` named strategies. Whether the standalone flags get deprecated, kept for backward compatibility, or treated as aliases is an open question deferred until operators have used both paths for a cycle or two.

## Worktrees

Worktree lifecycle — the `epic/` integration branch and per-task `cycle/`
working branches, their creation, fan-in merge, retention of failed lanes, and
full teardown on plan completion — is **model-driven** for both sequential
worktree isolation and `parallel-fanout`. The deterministic bookkeeping lives in
the spawn-free `workflows/parallel-dispatch.lua` seam: `cycle_plan` computes one
sequential lane, while `plan`/`waves` compute parallel lanes. The model runs the
git ops and spawns the coders. There is no external harness in this path. The
branch/path naming scheme is locked in the tech spec (epic
`epic/p<plan>-<slug>`, lane `cycle/p<plan>/<task-slug>`); the six
parallel-eligibility rules are the engine's (`recommend-strategy`) and apply
only to `parallel-fanout`, never to a single sequential worktree lane.

The runtime invariant below governs scope whenever a process runs with cwd
inside a worktree.

## Scope inside worktrees — parent repo dictates, planning verbs refused

Two invariants govern scope behavior when cwd is inside a worktree:

1. **The parent repo always dictates the scope.** A worktree at `<repo>/.worktrees/{epic,cycle}/...` resolves to the same association as `<repo>`. Worktrees are not separately scoped entities; they inherit. Reads (`planar plan list`, `planar task show`, `planar-watch *`) work transparently from inside a worktree and see the parent's data.
2. **Planning verbs are refused from inside worktrees.** The runtime entry point refuses the planning-class verb set with a distinct exit code and a message pointing at the parent repo's cwd.

**Planning-class verbs (refused in worktree):** `init`, `plan {add,update,done}`, `task {add,update,done,touches}`, `question {add,answer,wontfix}`, `decision {add,accept,reject}`, `artifact {add,update}`, `scenario {add,update}`, `spec {draft,ingest}`, `link`, `unlink`, `links {add,remove}`, `assoc {add,update}`, `promote`, `demote`.

**Execution / read (allowed in worktree):** every `planar-agent *`, every `planar-watch *`, `planar resume`, `planar dashboard`, `planar handoff *`, `planar capture *`, `planar audit *`, `planar health`, every `* show` / `* list` read, `planar workbench {pull,push,status,sync,resolve}`, `planar workspace *`.

**`task done` is refused on purpose.** Outside the explicitly selected in-pwd
`barrel-bypass` exception, coders return evidence and the orchestrator advances
task state with `planar-agent complete --claim <token>`, the atomic terminal
verb that flips claim status and task status in one transaction. This
reinforces the four-binary boundary and the canonical claim ritual in
[Coordination claims](#coordination-claims).

**`--scope <slug>` does NOT override the refusal.** The rule is about *where the verb runs*, not which scope it targets. To plan against a member repo from elsewhere, cd to the parent repo (or workspace root with `--scope <member>`); do not try to plan from inside a worktree.

Planar's runtime classifies the verb and refuses planning verbs from inside a
worktree with its documented distinct exit code. Worktree detection resolves
the parent repo from the managed worktree layout and Git metadata, so the
parent-dictates-scope rule holds whether or not the worktree lives physically
under the repo. Scope resolution is cwd-first: the literal cwd's registered
project wins if one exists; otherwise it falls back to the Git-derived parent
repo's scope.

## Dispatch mode selection

Dispatch has two orthogonal axes. They are independent and must not be conflated.

### Axis A — Isolation (non-negotiable)

Every source-content mutation happens inside a **freshly spawned write
specialist** with blank context. This is independent of file count, edit
triviality, and model tier.

The orchestrator never authors source content. It may run coordination CLIs and
the documented Git topology operations for worktrees, commits, fan-in, and
cleanup. "Dispatch" means spawning through the host's subagent surface, never
invoking `/pl-coder` inline in the orchestrator's context.

Isolation is always-on for source authoring. Coordination and Git topology
operations remain the orchestrator's responsibility.

### Axis B — Reviewer disposition (tunable)

Whether a reviewer pass runs after the coder is a separate, per-cycle decision. The six dispatch shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`) and the [Reviewer dispatch profile](#reviewer-dispatch-profile) below are entirely Axis B. All six shapes must be preserved and are not affected by the isolation rule.

The default is **review**. Skip reviewer dispatch only when the operator
explicitly selects `barrel-bypass`; the orchestrator never infers that choice
from file count, task labels, or apparently mechanical work. A profile may
record `skipped-by-profile` for a cycle that performs no repository mutation,
such as recording an already approved Planar decision, but source-content
changes remain reviewer-on unless explicitly bypassed.

Before accepting an explicit bypass, confirm that every required validation
profile entry passed and that the profile covers every changed surface. A
missing, flaky, skipped, or invented validator blocks the bypass.

### Axis C — Tier (separate from A)

The coder subagent defaults to `medium` tier; Planar's decomposition-first
premise means execution is deliberately economical, and the default is expected
to hold for most tasks. The orchestrator may propose `large` per task for
schema changes, genuine engine-judgment calls involving transactional,
concurrency, ownership, security, state-transition, or resource-lifecycle
boundaries, or large architectural work — but tier is per-task and independent
of isolation. A cycle never inherits its highest task's tier: a mixed-tier
group is partitioned by tier or dispatched per task. Classification ambiguity
is surfaced at the gate as `tier: ?`, never rounded up silently. See
[`agents/models.md` §Coder tier policy](models.md#coder-tier-policy).

## Common defects pre-flight checklist

Run this checklist before every work-complete declaration. The orchestrator
first derives a validation profile from the target repository's own documented
surfaces: contributor guidance, manifest or task-runner definitions, CI
workflow, package metadata, and changed subsystem instructions. It presents the
profile at the strategy gate and the operator confirms or edits it. Global
orchestration definitions name gate classes, never programming languages or
target-specific commands.

- [ ] Every required validation-profile entry ran with its exact confirmed
      command and returned a recorded exit status.
- [ ] The profile covers formatting/static analysis, build or packaging,
      focused verification, broader regression verification, generated/rendered
      artifact parity, and repository-specific policy checks when those classes
      exist for the changed surfaces.
- [ ] Bulk or mechanical edits were followed by the relevant format, search,
      and completeness validators from the confirmed profile.
- [ ] For any reference to a schema value, confirm it in the target
      repository's authoritative schema or migration source; never guess from
      memory or assume a directory layout.
- [ ] For any documentation table (especially in ADRs or tech-specs):
      cross-check each row against the authoritative source. Tables that
      summarize values from other documents must be validated against those
      documents, not just internally consistent.
- [ ] No references to files that have been renamed or deleted (grep for
      all file paths cited in changed documents).

## Failures that impersonate regressions

Some failures are environmental, look exactly like a code regression, and
invite a reflex that makes things worse. Three cost real debugging time in
this repo before anyone checked the environment instead of the diff (tasks
6311, 6315, 6350). **Check the environment before you bisect.**

| Signature | Actual cause | The tell |
|-----------|--------------|----------|
| Mass `conn.has_value() == false`, reading as a database-layer regression | Disk exhaustion — the suite had leaked temp arenas until the volume filled (task 6311) | `df` the volume. A real DB regression does not fail every connection in the suite at once. |
| Exit 138, zero diagnostics, reading as "the known flake" | A doxygen SIGBUS retry loop masking a genuine failure — it hid one three times (task 6315) | Signal death is not a lint verdict. Distinguish a signal exit from a non-zero *diagnostic* exit before retrying. |
| `39 failed`, large `NOT_BUILT` population | Two builds racing in one build directory; `clang-scan-deps` lost a temp-file rename to a concurrent ninja (task 6350) | **Which exit code is non-zero.** `BUILD_EXIT != 0` with `NOT_BUILT` tests means the suite never ran. A real regression gives `CTEST_EXIT != 0` with *named* failing tests. |

The third one generalizes past its own signature, and that is the part worth
carrying forward. A historical variant — two `test-parity-cpp` runs sharing
one `.zig-cache`, both since deleted at the M10 cutover — built cleanly, ran,
and reported `276 CRASH` out of 652 where the truth on the same commit was 18.
There was **no exit-code tell at all**, and the reflex it invited was not
re-running but bisecting, or reverting a merge that was never at fault.

So do not treat the `BUILD_EXIT` tell as the general rule. It is the tell for
one variant. The general rule is the operational one:

**One build at a time per build directory.** That covers `make cpp-lint`
(it builds) against `ctest` (it builds), either against
`scripts/break-probe.sh` (it rebuilds per mutation), and any of those against
an orchestrator running its own verification in the same worktree. The last is
the one that actually bit: the coder and the orchestrator were both running
gates against one tree. Pick one — either the lane reports its number and the
orchestrator re-measures after merge on a quiet tree, or the orchestrator
tells the lane not to run the gate at all. See also
[Probes come before gates, and never beside them](#probes-come-before-gates-and-never-beside-them).

## Break-probe discipline

A break-probe is the standing evidence that a new test discriminates:
mutate the implementation, confirm the NAMED test fails, restore, confirm
it passes again. A test authored alongside the code it covers can pass
vacuously, and a suite that has never been shown to fail proves nothing.

Every step of that sequence has a silent-failure mode, and each one looks
exactly like success:

- **The restore leaves an OLDER mtime.** `cp F F.bak; <mutate>; build;
  <test>; mv F.bak F` puts back the backup's original timestamp.
  Incremental build systems that compare mtimes (ninja, make) see nothing
  newer than the object file, skip the rebuild, and leave the MUTANT
  linked — so every probe after the first in that pass measures the wrong
  binary while reporting "all mutants killed". Always `touch` the file
  after restoring, rebuild, and re-run the test to prove the restore took.
- **The anchor matched zero occurrences.** A mutation that changed nothing
  produces a green test that is indistinguishable from a survivor. Verify
  the substitution count, or diff the file against the backup.
- **The mutant did not compile.** A compiler-rejected mutant proves the
  compiler works, not that the test discriminates. It is not a kill.
- **The test filter matched nothing.** Confirm the filter names real test
  names, not framework tags, and that it selected at least one.
- **The mutation changed the file but not the behaviour.** A substitution
  that lands on a comment, on whitespace, on dead code, or inside an
  unreachable branch passes the zero-match check above and still compiles
  to a semantically identical binary. No test can catch it, so the green
  result says nothing — but it looks exactly like a survivor. Confirm the
  mutation actually changed compiled output before believing any verdict
  about the test.

A survivor is a finding, not a footnote: either the test needs rebuilding
around what is actually observable, or the mutation is provably equivalent
— and "provably" means the difference was measured, not argued.

An **inert** mutant is a different finding from a survivor, and conflating
them is how a test acquires evidence it never earned. A survivor indicts
the test; an inert mutant indicts the probe and must be re-aimed and
re-run before the test is judged at all.

In the Planar repository the C++ tree ships `scripts/break-probe.sh`,
which enforces all five checks around one probe and reports
`killed` (exit 0) / `SURVIVOR` (exit 1) / `INERT` (exit 3), with exit 2
reserved for a broken probe. Prefer it over a hand-run sequence.

### Probes come before gates, and never beside them

Break-probes are the one piece of evidence only the coder can produce; the
validation gates are reproducible by anyone downstream. Ordering them the
other way round is what actually loses the evidence. A full gate pass here is
roughly 25 minutes, so a coder that runs gates in the foreground and blocks
runs out of turn before it reaches its probes — five coder stops across tasks
6339 and 6343 were all that shape (task 6346). The orchestrator then inherits
tests with unproven discriminating power, and on 6339 the reviewer ran the
probe itself: the right outcome from the wrong role, because a reviewer judges
evidence rather than manufacturing it.

Run probes first, then background the long gates and keep working. Do NOT run
the two concurrently against one build directory: a probe rebuilds, the suite
reads what it rebuilt, and the resulting failures look real while carrying no
exit-code tell (task 6350). Give the probes their own build directory or
sequence them strictly before the suite starts.

## Dispatch Granularity

Tasks created by the ingestor are deliberately fine-grained: one roadmap bullet → one task row. That granularity is correct for *tracking* but is often wrong as a coder→reviewer iteration unit — eight tasks that all touch the same helper file are naturally a single PR, not eight separate review cycles. Conversely, some users want strict one-task-per-commit history for easy bisect and rollback. The right shape is a per-feature judgement call, not a fixed policy.

Before Phase 3 dispatch the orchestrator analyzes task coupling (shared file scope, sequential dependencies, doc-only deltas) and proposes a **dispatch shape**:

| Shape              | Reviewer disposition          | Pick when |
|--------------------|-------------------------------|-----------|
| `strict`           | Per task (full reviewer)      | Fine-grained history, easy bisect/rollback, parallel review of independent tasks. |
| `grouped`          | Per group                     | Tasks share file scope and a single review naturally covers them all. |
| `single`           | Once                          | Tiny features where decomposition is theatre. |
| `barrel-grouped`   | Per group                     | Alias for `grouped` with the milestone heuristic locked in; explicit barrel namespace. |
| `barrel-deferred`  | Deferred (at boundary)        | Throughput priority with a late safety net. Coder cycles run back-to-back; reviewer fires once at a milestone (default) or plan boundary on the union diff. |
| `barrel-bypass`    | None                          | Maximum throughput. Quality gates ARE the review signal. No reviewer dispatch at all. |

Groups must be **tier-homogeneous**: a `grouped`/`single` cycle whose tasks carry different confirmed Axis C tiers is partitioned by tier before dispatch — one coder per tier partition, dependency edges still ordering the dispatches — or falls back to per-task dispatch for that cycle. A cycle never inherits its highest task's tier (see [`agents/models.md` §Coder tier policy](models.md#coder-tier-policy)).

The orchestrator **surfaces the proposed shape to the user and waits for confirmation** before dispatching. This is a hard user gate identical in strength to the Phase 2 ingestion gate. The user may accept the proposal, edit the groupings, or pick a different shape entirely. The gate is bypassed only when the invocation already specifies `--strict`, `--grouped`, `--barrel-grouped`, `--barrel-deferred [--barrel-deferred-at milestone|plan]`, `--barrel-bypass`, or `--batch <task-ids>...` — those flags act as a pre-committed answer.

The iteration cap (5) applies per **cycle**, not per task: a `grouped` cycle that fails on iteration 5 surfaces the whole group with caveats or aborts. Under `barrel-bypass` the iteration cap is *undefined* — there is no reviewer dispatch, so no `request-changes`, so no iteration. The coder ships one iteration and that is the entire cycle.

The three `barrel-*` shapes are documented in detail in §[Barrel modes](#barrel-modes) below.

## Iteration Loop

- **Cap: 5.** A dispatch cycle (one task under `strict`, a group of tasks under `grouped`/`single`) may go through at most five coder iterations against the same reviewer.
- **Counter** is per-cycle, incremented on each coder→reviewer cycle.
- **Iteration 5 (the final permitted iteration):** `request-changes` is *not* a valid outcome. The reviewer must choose between `approve` (ship-as-is, with caveats explicitly documented) and `abort` (escalate to user with WIP state).

The cap is hard-coded for now. See [Things To Revisit](#things-to-revisit).

### Iteration 5 contract

The 5-iteration cap exists, but the iter-5 dispatch contract is load-bearing
on its own and was under-specified before. The reviewer dispatched on
iteration 5 operates under different rules than iterations 1–4:

- **`request-changes` is invalid on iteration 5.** The next reviewer
  dispatch MUST return `approve` (with caveats) or `abort`. A reviewer
  who returns `request-changes` on iteration 5 has produced an invalid
  decision and the orchestrator treats it as `abort`.
- **Caveats are tracked as new task rows under the same plan**, not
  buried in commit messages or a "deferred" section of the report.
  Ship-with-caveats means the work merges AND a follow-up task row
  exists for each caveat (`planar task add --plan <p> ...`), so the
  caveat re-enters the queue rather than being lost.
- **Abort escalates to the user and halts the cycle.** The orchestrator
  preserves WIP state and surfaces the abort reason; the cycle does not
  resume without user intervention. Abort is the correct decision when
  ship-with-caveats would require caveats large enough that they belong
  in the next cycle rather than as follow-ups.

## Barrel modes

The three `barrel-*` shapes formalize what was previously an emergent shortcut: barrel through milestones back-to-back, accept the gates as the entire signal, and defer (or skip) the reviewer. Naming them turns the shortcut into a contract — the operator picks at the gate, the audit trail records the choice, and the reviewer-skip decision is explicit rather than emergent.

Each barrel mode picks a distinct point on the *grouping* × *reviewer disposition* matrix. They are **not** mutually exclusive with `strict`, `grouped`, or `single` — they are siblings on the same gate. The operator picks one shape per `/pl-orchestrator` invocation against one anchor plan.

### `barrel-grouped`

An alias for `grouped` with the milestone heuristic locked in. The orchestrator picks groupings by milestone boundary (one cycle per child plan) unless the operator overrides via `--batch`. Each group still gets its own reviewer dispatch per the existing dispatch profile.

The alias exists for clarity and audit-trail searchability — `barrel-grouped` in the session entry signals "the operator explicitly picked the barrel namespace," distinct from `grouped` which was the orchestrator's coupling-analysis recommendation.

### `barrel-deferred`

Coder cycles run back-to-back without reviewer dispatch between them. The reviewer is dispatched **once at a configurable boundary** on the union of queued diffs:

- `--barrel-deferred-at milestone` (default) — reviewer fires at the close of each child plan.
- `--barrel-deferred-at plan` — reviewer fires once at the end of the entire plan, across all milestones.

Per-milestone is the default because per-plan can produce a union diff too large for the reviewer to read carefully. Plan-level is an opt-in for short plans (≤2 milestones) or plans where the operator is confident the cumulative diff stays small.

The iteration-5 cap applies **per reviewer dispatch**, not per cycle queued under it. A reviewer dispatched under `barrel-deferred` may iterate up to 5 times against the union diff. `request-changes` on iteration 5 is invalid and treated as `abort` — the same rule as for `strict`/`grouped`/`single`. Abort under `barrel-deferred` halts the entire queue of cycles, which is the operator's incentive to not pick over-aggressive deferred boundaries.

The orchestrator's queue of pending cycles is recoverable from `session_entries` (each cycle records a `prefix='note'` entry whose body begins with the sentinel line `dispatch_shape: barrel-deferred` and includes `reviewer_disposition: deferred`).

### `barrel-bypass`

No reviewer dispatch occurs. Coder cycles run back-to-back; every required
entry in the target repository's confirmed validation profile is the entire
signal. The iteration cap is undefined because there is no reviewer loop.

This mode is supported only as an explicit expert choice and is never the
orchestrator's recommendation. The coder returns a structured evidence row for
each profile entry: validation id, exact command, required flag, exit status,
result, and artifact or bounded citation. A missing, skipped, flaky, or failed
required entry blocks completion. No universal command, language, or repeat
count is implied; repetition is required only when the confirmed profile says
so. A coder dispatched under `barrel-bypass` must be told that the operator
selected this risk profile explicitly.

Operators picking `barrel-bypass` accept that uncaught defects must surface via runtime testing or out-of-band review. The closest retroactive surface is `git blame` + `/pl-reviewer <task-id> <iteration>` against a still-active task; there is no orchestrator-driven "review this old cycle" workflow.

### Phase 3.5 composition (all barrel modes)

Phase 3.5 (test-coder dispatch) fires across **all** barrel modes when uncovered slugs intersect the cycle's dispatched slugs. `barrel-bypass` bypasses the *reviewer*, not the *coverage gate* — the test-coder's role is the coverage check, not the review pass.

Concretely:
- Under `barrel-grouped` / `barrel-deferred`: Phase 3.5 runs per-coder-cycle (same as `grouped`).
- Under `barrel-bypass`: Phase 3.5 still runs per-coder-cycle. The `failure-surfaced` outcome still halts the cycle and escalates to the operator. The "bypass" applies to the downstream reviewer pass, not to the upstream test-coder gate.

Operators who want to skip Phase 3.5 specifically can either (a) remove the `[slug:]` annotations from the relevant roadmap bullets so the gate doesn't fire on them, or (b) edit the test-spec to drop the `task:<slug>` citation. Both edits are explicit and audit-trail-visible. There is **no** `--skip-phase35` flag.

### Audit trail

Every cycle the orchestrator dispatches MUST append a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `dispatch_shape: <shape>`. The body uses a stable line-oriented schema so `planar audit trail --kind plan <plan-id> --grep "^dispatch_shape:"` recovers the per-cycle disposition reliably:

```
dispatch_shape: <one of: strict|grouped|single|barrel-grouped|barrel-deferred|barrel-bypass>
reviewer_disposition: <one of: dispatched|skipped-by-profile|deferred|bypassed>
cycle_scope: <plan:N milestone:M | task:T...>
tasks: [<id>, <id>, ...]
claim_tokens: [<token>, <token>, ...]
```

The `reviewer_disposition` field captures the precedence rule: `barrel-bypass` mode overrides the per-cycle reviewer-skip profile and records `bypassed` (not `skipped-by-profile`). This distinguishes "operator chose to bypass" from "this cycle shape had no review signal anyway."

`planar audit trail --kind plan <plan-id>` exposes the prefix and body for forensic recovery. No schema change; `session_entries.prefix` is CHECK-constrained to a fixed set (`action / observation / decision / question / file / command / note / error / read`), so the dispatch convention reuses `prefix='note'` with the sentinel body line as the grep-recoverable alternative.

### Phase 3.5 — Test-coder dispatch

Between the coder's report-done and the reviewer's dispatch, the orchestrator may dispatch a [`test-coder`](test-coder.md) cycle. The gating decision is delegated to `planar test-spec status <plan> --json` — the orchestrator does NOT re-implement coverage calculation.

**Gating condition.** Dispatch test-coder when:
- The cycle's dispatched tasks carry `[slug: …]` annotations, AND
- `planar test-spec status --json` reports `uncovered_task_slugs` non-empty intersection with the cycle's slugs.

Tasks without a `[slug:]` annotation are out of scope for the test-coder by construction; they appear in the `tasks_without_slug` count, not the gate. When the intersection is empty, skip Phase 3.5 entirely and dispatch the reviewer directly.

**Iteration cap.** The test-coder cycle has its own cap (default 2; the work shape is "expand or don't" rather than "iterate to convergence"). The coder/reviewer 5-iteration cap is unaffected by the test-coder's cap.

**Outcomes.**
- `expanded` — orchestrator stages the test-coder's diff alongside the coder's; dispatches the reviewer with the union diff.
- `no-expansion-needed` — orchestrator dispatches the reviewer normally; the coder's diff is the entire work.
- `failure-surfaced` — orchestrator escalates to the operator with the test-coder's report (including the `recommendation` field on each failing test). The reviewer is NOT dispatched until the operator resolves.
- `abort` — orchestrator escalates; cycle halts.

**Diff base.** The orchestrator records HEAD at coder dispatch
(`<coder-cycle-base>`) and passes it to test-coder and reviewer. Both run `git
status --short`, `git diff <coder-cycle-base>`, and `git diff --stat
<coder-cycle-base>`. Comparing the base to the working tree covers committed
and uncommitted cycle changes; `git diff HEAD` and `<base>..HEAD` alone are
forbidden because either can be empty for a valid cycle shape.

## Reviewer dispatch profile

The reviewer is load-bearing for some cycle shapes and pure overhead for
others. The orchestrator picks a reviewer disposition per cycle, not per
feature. Default is reviewer-on. Repository mutations skip review only through
the operator's explicit `barrel-bypass` choice.

| Cycle shape | Reviewer disposition | Why |
|-------------|----------------------|-----|
| Architectural / handoff cycles | Load-bearing — always dispatch | Cross-cutting choices need a fresh, independent read against the spec. |
| Schema migrations | Load-bearing — always dispatch | Forward + back safety, FK implications, index correctness all require a second pair of eyes. |
| New CLI surfaces | Load-bearing — always dispatch | Flag semantics, error messages, `--json` shape, exit codes harden against an independent read. |
| Validate / invariant changes | Load-bearing — always dispatch | Each rule must be covered by a test and the reject paths must return useful errors. |
| Refactor sweeps with semantic implications | Load-bearing — always dispatch | Not pure mechanical — behavior may shift under the rename. |
| Single-feature additions (the middle ground) | Reviewer-on | The middle defaults to independent review. |
| Decision-only cycles (recording `planar decision add` rows) | Skip — reviewer adds no signal | Reviewer cannot verify decision content beyond what the operator already approved. |
| Pure mechanical sweeps | Reviewer-on unless operator explicitly selects bypass | Mechanical appearance does not prove semantic safety. |
| Docs changes | Reviewer-on unless operator explicitly selects bypass | Documentation can encode contracts and executable examples. |

When the reviewer IS dispatched, see the role spec at
[`agents/reviewer.md`](reviewer.md) for the focused responsibilities and
the explicit NOT-do list. The blind-read contract below governs how the
brief is composed.

### Blind-read contract

The reviewer brief MUST NOT include the coder's report. A reviewer who
reads the coder's narrative inherits the coder's framing — the exact
framing the coder may have rationalized past. The reviewer sees only:
the task IDs and claim tokens the coder claimed, `<coder-cycle-base>`, the
relevant spec/roadmap section paths (not bodies), a directive to run `git
status --short`, `git diff <coder-cycle-base>`, and `git diff --stat
<coder-cycle-base>` themselves, the confirmed validation profile, and the
coder's structured validation evidence. The orchestrator
preserves this contract by composing a fresh brief.

## Brief composition discipline

The dispatcher's brief is the input the coder runs on. Sloppy briefs are
not a coder problem to recover from — they are a dispatcher problem to
prevent. This applies to whoever composes the brief: the orchestrator
agent dispatching a coder cycle, or a human-in-the-loop assembling a
brief by hand.

Every coder brief MUST:

- **Cite spec section paths, not paraphrased spec content.** Point at
  the workbench tech-spec section the coder needs to read; do not
  summarize it in the brief. A paraphrase loses load-bearing detail and
  becomes the coder's source of truth in place of the spec. The brief
  is a pointer.
- **List task IDs explicitly.** The cycle's scope is the enumerated task
  list. "Implement the parser milestone" is not a scope; "tasks 1247, 1248, 1249 under
  the parser milestone" is. Task IDs are the contract the reviewer compares
  the diff against.
- **List claim tokens explicitly.** The cycle's synchronization scope is
  the claim token set. The reviewer uses it to verify the diff stayed
  inside leased work, and an interrupted session uses it to resume or
  reconcile stale ownership.
- **Note locked decisions inline.** Patterns like `Q47 = editor markers`
  or "ADR-0012 forbids new global state here" go in the brief verbatim,
  so the coder does not re-litigate them. A decision restated in the
  brief is faster to honor than one buried in an ADR the coder may not
  reach.
- **Specify the confirmed validation profile.** List an id, exact command,
  required/optional status, changed surfaces covered, and any required repeat
  count for every entry. The profile comes from the target repository's own
  guidance and automation; the orchestrator does not invent commands from a
  language or framework assumption.
- **Specify the report shape.** A word ceiling (the work-complete
  report has a cap — keep it tight) and the required sections per
  [`agents/coder.md` §Work-complete report template](coder.md#work-complete-report-template).
  The dispatcher reads the report; vague shape produces vague reports.
- **Pose the problem; do not include the solution.** State the
  invariant, the constraint, and the acceptance signal. Let the coder
  design the implementation. A brief that prescribes the diff turns the
  coder into a transcriber and removes the signal that the design step
  produces.
- **Cite the test-spec section paths when the dispatched tasks have
  `verifies` edges to test-spec scenarios.** The test-spec
  is a planning artifact at the same level as the product- and
  tech-spec; when scenarios are cited against the task IDs in the
  cycle, the brief must point the coder at the relevant test-spec
  sections so the test plan informs implementation choices. List the
  cited scenario IDs separately in the brief — the reviewer compares
  the diff against them. When no scenarios are cited the test-spec
  reference can be omitted.

The dispatcher does not paraphrase the spec into the brief. The brief
is a pointer to the spec, not a substitute for it. A coder who works
from the brief alone inherits the dispatcher's compression of the spec,
and the reviewer cannot recover the loss after the fact.

## Reviewer Decisions

| Decision           | Meaning | Next step |
|--------------------|---------|-----------|
| `approve`          | Output meets the task acceptance signal. On iteration 5 specifically, may be approved with caveats — those caveats are recorded as decisions or follow-up tasks. | Orchestrator invokes `planar-agent complete` for each approved claim and proceeds to the next dispatch. |
| `request-changes`  | Output is salvageable but needs specific edits. Reviewer attaches concrete remediation. Not valid on iteration 5. | Orchestrator returns to coder for the next iteration. Iteration counter increments. |
| `open-question`    | Reviewer cannot reconcile a question without user input (ambiguous spec, scope conflict, missing decision). The reviewer must first attempt reconciliation against the spec, ADRs, and prior decisions; this decision is reserved for genuine blockers. | Orchestrator pauses the task and surfaces the question to the user. The task resumes when the user answers. |
| `abort`            | The task as currently scoped should not proceed. Reviewer documents why. | Orchestrator escalates to the user with the abort reason and WIP state. The task does not resume without user intervention. |

## Escalation

1. **Open question (any iteration).** The reviewer must first attempt reconciliation against the spec, ADRs, and prior decisions. Only when reconciliation genuinely fails does the question become an `open-question` decision routed through the orchestrator to the user.
2. **Iteration 5 reached without `approve` on iterations 1–4.** The reviewer chooses `approve` with caveats or `abort`. Either way, the orchestrator produces a user-facing summary: what was attempted, what blocked acceptance, and the recommended next step.
3. **Abort (any iteration).** The orchestrator preserves the WIP state and surfaces it to the user. The task does not resume without user intervention.

## Concurrency

- Parallel coder dispatch is **model-driven under `parallel-fanout`** (plan 760): the model spawns N coders concurrently through the host's subagent dispatch surface, each in its own worktree, against the parallel-eligible subset. The codified eligibility test (six parallelizability rules) is the engine's, consumed via `recommend-strategy` and never re-derived.
- **Eligibility is not independence.** Rule 2 proves the lanes touch disjoint files; it cannot prove they are unordered. Two tasks routinely have disjoint touch sets *and* a real dependency — one creates a module the other imports, so the file sets never overlap because only the author edits the new file. Rule 1 catches ordering only from a declared `depends-on` edge, and nothing infers those edges: touch inference improves rule 2 alone. As touch coverage rises, more plans will *look* parallelizable while undeclared ordering stays invisible, so the orchestrator's pre-dispatch ordering check (Phase 3 step 3) is what stands between eligibility and a correct fan-out. Record any ordering the operator names with `planar task block <task> --on <blocker>` so rule 1 enforces it thereafter. Sequential strategies (`classic` and the barrel modes) run one coder at a time in the confirmed isolation mode: `pwd` or `worktree`.
- The substrate that lets worktree-isolated coders run without clobbering the operator checkout — per-task worktrees on epic-child branches, staged waves with a fan-in barrier for `parallel-fanout`, and the fan-in merge — is computed by the spawn-free `workflows/parallel-dispatch.lua` seam and executed by the model. See [Worktrees](#worktrees) for the ownership boundary.
- Reviewers may run in parallel against independent coder outputs. Under `parallel-fanout` a single reviewer cycle runs against the integrated diff on the epic, not per child.
- A single task is always coder→reviewer sequential — never two coders on the same task simultaneously.
- Stale claims are not ignored silently. The operator or orchestrator must reconcile or force-takeover them before treating the work as available.

## State Capture

State capture lives in SQLite per the locked schema. Tasks carry `next_action`. `sessions` and `session_entries` capture the iteration timeline. `agent_work_claims` records live ownership and lease state. `agent_actions` records typed time-bounded work inside sessions. `decisions` records reviewer rulings. Open questions are `question` rows linked to the task. Aborts surface as session entries with `prefix='error'` linked to the task and the originating decision, and the corresponding claim is released as `aborted` or later reconciled as `stale`.

## Heartbeat status contract

Agents communicate their current activity to the read surface (`planar-watch ps`, `planar-watch feed`) by supplying an optional `--status <text>` string on every heartbeat call:

```
planar-agent heartbeat --claim <token> --status "editing <module-or-area>"
```

When `--status` is omitted, the heartbeat refreshes the lease without changing the displayed activity. Status strings are **free-form text** — there is no state machine enum. Agents choose strings that describe what they are doing in human-readable terms.

### The `awaiting:` prefix convention

When an agent is blocked on an external event — a downstream sub-agent returning, an operator answering a question, an external API responding — it prefixes the status string with `awaiting:`:

```
planar-agent heartbeat --claim <token> --status "awaiting:coder"
planar-agent heartbeat --claim <token> --status "awaiting:reviewer"
planar-agent heartbeat --claim <token> --status "awaiting:operator-confirmation"
```

The `awaiting:` prefix is a convention, not a parsed enum. The read surface (`planar-watch ps`) uses it as a color/sort signal but does not parse beyond the prefix boundary. Plain text (no prefix) means the agent is actively working.

### Per-phase-transition cadence rule

Heartbeat with a new `--status` string at every meaningful phase boundary:

- **claim-acquired** → `"claim acquired: task <id>"`
- **reading brief** → `"reading brief"`
- **editing files** → `"editing <module-or-area>"` (one status per area)
- **running validation** → one status per profile entry (`"validating: <gate-id>"`)
- **committing / reporting** → `"committing"` or `"reporting"` as appropriate

Heartbeats between phase transitions (lease-renewal-only) may omit `--status`. The cadence goal is: any operator watching `planar-watch ps` can tell what phase the agent is in without waiting for the next phase transition.

For long operations (> 30 s), heartbeat at least once per TTL/2 even if the status string does not change. A bare heartbeat renews the lease length already held, so no `--ttl` is needed to keep a long lease alive; pass `--ttl <secs>` only to change the lease length deliberately.

### Do not manually duplicate entity-create events

The engine hooks automatically write an `agent_actions` row when `question.create`, `decision.create`, or `artifact.create` fires under an active claim. Agents MUST NOT also call a separate `planar-agent heartbeat --status "created question X"` for the same event — that produces a duplicate action row and clutters the feed.

Status strings describe the **agent's own state** (what it is doing), not a mirror of entity-create events. Entity creates surface automatically; status strings are the agent's judgment about its current phase.

### 256-byte cap on `--status` payload

`planar-agent heartbeat --status` enforces a 256-byte upper bound on the status string. Strings longer than 256 bytes are rejected with `error.InvalidInput`. Keep status strings concise: a short phrase is enough for the feed to be readable.

### Cross-references

- Per-role canonical status strings: see the "Status reporting" sections in [`agents/coder.md`](coder.md#status-reporting), [`agents/orchestrator.md`](orchestrator.md#status-reporting), [`agents/planner.md`](planner.md#status-reporting), [`agents/reviewer.md`](reviewer.md#status-reporting), [`agents/test-coder.md`](test-coder.md#iteration-and-status), and [`agents/ingestor.md`](ingestor.md#status-reporting).
- `agent_actions` schema: see the installed Planar version's authoritative
  schema documentation.
- Claim ritual: see [Coordination claims](#coordination-claims) above.

## Things To Revisit

- **Iteration cap of 5.** Hard-coded today. Revisit once empirical data on real workloads exists — the right number may be 3, 5, or 8 depending on task shape. Move to the `config` table or methodology frontmatter if it needs to flex per project.
- **Per-tier reviewer.** Currently all reviewers are `large`-tier. Some review work may not need that; a cheaper review tier could be useful for routine tasks.
- **Parallelism heuristics.** The deliberately conservative
  parallel-eligibility rules live in Planar and are consumed via
  `recommend-strategy`; orchestration definitions do not reimplement them.
  Under-declared touches therefore over-serialize rather than creating false
  parallelism. Revisit once enough fan-out cycles have shipped.
- **Cross-vendor pairings.** A reviewer may be a different vendor than the coder (Claude reviewing Codex output, etc.). The methodology assumes this works; verify once cross-vendor handoff is exercised in real cycles.
- **Reviewer feedback format.** "Concrete remediation" is loose today. As patterns emerge, codify the structure (e.g. file:line + proposed change, or a structured issue list).
