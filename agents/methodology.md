---
name: methodology
description: Shared agent orchestration methodology — the flow, iteration loop, escalation rules, concurrency rules, and state capture that the orchestrator, coder, and reviewer agents collectively follow.
---

# Methodology

This document is the single source of truth for how Planar's agents collaborate. The `orchestrator`, `coder`, and `reviewer` specs link here as the authority on the iteration loop and escalation rules. Vendor surfaces (under `commands/claude/`, `skills/codex/`, `skills/copilot/`) inherit it transitively via their agent spec.

## Roles

| Agent          | Tier   | Role |
|----------------|--------|------|
| `orchestrator` | large  | Receives a task or task list from the user; dispatches to coders (in parallel where independent); routes coder output through reviewers; enforces the iteration cap; escalates to the user on open questions or abort. Owns the work-package as a whole. Does not write code, does not perform reviews. |
| `coder`        | medium | Implements one task end-to-end. Receives reviewer feedback on `request-changes` and addresses it in the next iteration. Does not approve its own work. |
| `test-coder`   | large  | Dispatched between the coder and the reviewer (Phase 3.5) when `planar test-spec status` reports uncovered slugs intersecting the cycle's dispatched tasks. Reads the test-spec + the coder's diff; writes tests against cited scenarios; surfaces failures (without modifying the test) with a recommendation. Never edits feature code. |
| `reviewer`     | large  | Reviews coder (and, when present, test-coder) output. Per iteration, decides one of `approve`, `request-changes`, `open-question`, `abort`. Does not implement fixes. |

Tier-to-model resolution: [`agents/models.md`](models.md).

## Phases

The orchestrator manages up to five phases per feature. Phases 1–2 apply only when the anchor plan is in `draft` status; Phases 3–5 apply for `active` and later statuses. Full phase documentation lives in [`agents/orchestrator.md`](orchestrator.md#phases).

| Phase | Skill | Trigger | User gate |
|-------|-------|---------|-----------|
| 1 — Planning | `pl-spec-draft` | Goal given; no anchor plan or `draft` with no artifacts | After artifacts are drafted — user reviews before Phase 2 |
| 2 — Ingestion | `pl-spec-ingest` | Anchor plan `draft` with workbench artifacts present | After preview diff — user confirms before `--apply` |
| 3 — Execution | coder + (optional test-coder) + reviewer | Anchor plan `active` or `paused` with `todo`/`doing` tasks | Dispatch-shape choice (strict/grouped/single) before kick-off; then per-cycle iteration loop (5-iteration cap). Phase 3.5 (test-coder dispatch) is conditional on `planar test-spec status`. |
| 4 — Propagation | `pl-ext-propagate` | User requests `--propagate` | Explicit per invocation |
| 5 — Archive | `pl-workbench-archive` | Anchor plan `done`, user requests `--archive` | Explicit per invocation |

**Key invariants:**
- The orchestrator never auto-applies ingestion. The ingestor always runs in preview mode first; `--apply` is gated on explicit user confirmation.
- The orchestrator never auto-archives. Archive is always an explicit user action.
- The orchestrator never picks dispatch granularity silently. Phase 3 begins with a strict/grouped/single proposal that the user must confirm (unless an explicit `--strict`/`--grouped`/`--batch` flag was supplied at invocation).
- For an `active` or `paused` anchor plan, the orchestrator skips Phases 1 and 2 and enters Phase 3 directly.
- Phase 3 work selection is claim-aware. Active, unexpired agent claims make a task or child milestone unavailable to new dispatch unless the operator explicitly forces stale-claim recovery.

## Plan-status invariant

Plan status is a function of task status, enforced at task-write time. This is the plan-status auto-promotion invariant (plan 304); the orchestrator does NOT need to explicitly walk child plans through `draft → active → done` after a barrel cycle.

The rule fires inside the transaction of every `task.Add`, `task.Update`, `task.Done`, `task.Reopen`, `task.Cancel`, and `task.Block`:

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

**Opt-out.** Each task verb accepts `--no-auto-promote` to skip the recompute for that single operation. Used by migrations and scripted bulk edits that don't intend the plan-level transition. Plan 304's adopter fix is the reference use-case: `--apply-removals` cancels tasks then explicitly abandons the parent plan, and the cancellation must NOT auto-promote (cancel→done would forbid the subsequent abandon).

**Audit trail.** Every transition emits a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `plan_status: <id>`. The `session_entries.prefix` CHECK constraint does not include a dedicated `status` prefix, so the sentinel-body convention is the grep-recoverable alternative. The same sentinel-body pattern applies to plan 298's `dispatch:` audit trail.

Recover the per-transition record with:

```
planar audit trail <plan> --grep "^plan_status:"
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

The claim ritual is:

1. **Check.** Before selecting work, run `planar plan next <plan>` or an equivalent claim-aware read. Do not derive next work from `task list --status todo` alone.
2. **Claim.** Before implementation, review, ingestion, or propagation work begins, acquire a claim for the exact task, task group, or child milestone with `planar agent claim`.
3. **Report.** Attach major role actions to the claim with `planar agent start` / `planar agent end` when the agent activity feature is available.
4. **Heartbeat.** Refresh the claim at least once per half-TTL while work continues. A long-running tool call may delay the heartbeat, but the agent should heartbeat immediately before and after such calls.
5. **Release.** End the cycle by releasing the claim as `completed`, `released`, or `aborted`. If the process dies, `planar agent reconcile` later marks the lease stale.

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

## Flow

1. **Intake.** The orchestrator receives one or more tasks (or a goal, or an anchor plan id). It determines the current phase based on the anchor plan's status and confirms scope and acceptance signal. Tasks it cannot unambiguously scope produce an `open-question` immediately and are paused.
2. **Claim-aware selection.** The orchestrator reads `planar plan next <plan>` and excludes active unexpired claims from runnable work. Stale claims are surfaced to the operator or reconciled explicitly before dispatch.
3. **Dispatch.** The orchestrator claims each runnable cycle, records claim tokens in the dispatch entry, and hands the claimed task(s) to a `coder`. Independent claimed cycles may run in parallel (see [Concurrency](#concurrency)).
4. **Implementation.** The coder heartbeats the claim while working, produces the change set, runs tests, reports back to the orchestrator.
5. **Review.** The orchestrator hands the change set and claim token(s) to a `reviewer`. The reviewer decides one of `approve`, `request-changes`, `open-question`, `abort` and treats edits outside the leased scope as scope drift.
6. **Loop or terminate.** On `request-changes`, the orchestrator returns to implementation with the same claim if the lease is still valid, or renews/reclaims explicitly. On terminal outcome, the claim is released as `completed`, `released`, or `aborted`. The loop is bounded by the iteration cap below.

## Dispatch mode selection

Use INLINE (skip reviewer dispatch) if ALL of the following hold:
  (a) ≤3 files modified
  (b) Only mechanical edits: renames, deletions, mass symbol-replace,
      comment-only changes, or whitespace normalization
  (c) Automated validation green pre-submit: `go fmt ./...` clean,
      `go vet ./...` clean, `go build ./...` clean, `go test ./...`
      green (or scoped to affected packages), project grep checks pass,
      render-check and any remaining relevant validators pass

Use STRICT (full coder + reviewer dispatch) if ANY of the following hold:
  - More than 3 files modified
  - Logic changes, new functions, new types, or new DB access
  - Spec, ADR, or migration changes
  - Reviewer failed the previous cycle on the same area

## Common defects pre-flight checklist

Run this checklist before every work-complete declaration. Add new entries
when a cycle surfaces a defect not already listed.

- [ ] `gofmt -l .` returns empty output (all Go files format-clean).
      Note: run this AFTER any bulk substitution (`sed`, find-replace).
      gofmt does not run automatically on substituted files.
- [ ] For any reference to a schema enum value (e.g. artifact kind, task
      status, plan status): confirm the value by reading the migration SQL
      (`src/migrations/`) — never guess from memory.
- [ ] For any documentation table (especially in ADRs or tech-specs):
      cross-check each row against the authoritative source. Tables that
      summarize values from other documents must be validated against those
      documents, not just internally consistent.
- [ ] `go vet ./...` returns no diagnostics.
- [ ] `go build ./...` succeeds.
- [ ] `go test ./...` green (or scoped to modified packages).
- [ ] `make render-check` passes if any skill/agent surface was modified.
- [ ] No references to files that have been renamed or deleted (grep for
      all file paths cited in changed documents).

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

No reviewer dispatch at all. Coder cycles run back-to-back; quality gates (`gofmt`, `go vet`, `go build`, `go test`, two-run integration, `make render-check`, plus any remaining relevant validators) are the entire signal. The iteration cap is *undefined* — there is no reviewer, so no `request-changes`, so no iteration.

The contract under `barrel-bypass`: the coder's quality-gate output IS the review. The coder must paste gate citations verbatim in the work-complete report, run the integration suite twice, and (if vendor/agent surfaces are touched) run `make render-check` and any remaining relevant validators. A coder dispatched under `barrel-bypass` must know its situation; the coder agent spec acknowledges this contract explicitly.

Operators picking `barrel-bypass` accept that uncaught defects must surface via runtime testing or out-of-band review. The closest retroactive surface is `git blame` + `/pl-reviewer <task-id> <iteration>` against a still-active task; there is no orchestrator-driven "review this old cycle" workflow.

### Phase 3.5 composition (all barrel modes)

Phase 3.5 (test-coder dispatch) fires across **all** barrel modes when uncovered slugs intersect the cycle's dispatched slugs. `barrel-bypass` bypasses the *reviewer*, not the *coverage gate* — the test-coder's role is the coverage check, not the review pass.

Concretely:
- Under `barrel-grouped` / `barrel-deferred`: Phase 3.5 runs per-coder-cycle (same as `grouped`).
- Under `barrel-bypass`: Phase 3.5 still runs per-coder-cycle. The `failure-surfaced` outcome still halts the cycle and escalates to the operator. The "bypass" applies to the downstream reviewer pass, not to the upstream test-coder gate.

Operators who want to skip Phase 3.5 specifically can either (a) remove the `[slug:]` annotations from the relevant roadmap bullets so the gate doesn't fire on them, or (b) edit the test-spec to drop the `task:<slug>` citation. Both edits are explicit and audit-trail-visible. There is **no** `--skip-phase35` flag.

### Audit trail

Every cycle the orchestrator dispatches MUST append a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `dispatch_shape: <shape>`. The body uses a stable line-oriented schema so `planar audit trail <plan> --grep "^dispatch_shape:"` recovers the per-cycle disposition reliably:

```
dispatch_shape: <one of: strict|grouped|single|barrel-grouped|barrel-deferred|barrel-bypass>
reviewer_disposition: <one of: dispatched|skipped-by-profile|deferred|bypassed>
cycle_scope: <plan:N milestone:M | task:T...>
tasks: [<id>, <id>, ...]
claim_tokens: [<token>, <token>, ...]
```

The `reviewer_disposition` field captures the precedence rule: `barrel-bypass` mode overrides the per-cycle reviewer-skip profile and records `bypassed` (not `skipped-by-profile`). This distinguishes "operator chose to bypass" from "this cycle shape had no review signal anyway."

`planar audit trail <plan>` exposes the prefix and body for forensic recovery. No schema change; `session_entries.prefix` is CHECK-constrained to a fixed set (`action / observation / decision / question / file / command / note / error / read`), so the dispatch convention reuses `prefix='note'` with the sentinel body line as the grep-recoverable alternative — the same convention used by plan 304's `plan_status:` audit trail.

### Phase 3.5 — Test-coder dispatch

Between the coder's report-done and the reviewer's dispatch, the orchestrator may dispatch a [`test-coder`](test-coder.md) cycle. The gating decision is delegated to `planar test-spec status <plan> --json` (plan 286 M4) — the orchestrator does NOT re-implement coverage calculation.

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

**Diff base.** The orchestrator records HEAD at coder dispatch (`<coder-cycle-base>`) and passes it to the test-coder as the diff base for `git diff <coder-cycle-base>..HEAD`. The DB-driven gating decision (above) uses the post-coder-apply DB state, not the git ref — the ref is only the test-coder's reading material.

## Reviewer dispatch profile

The reviewer is load-bearing for some cycle shapes and pure overhead for
others. The orchestrator picks a reviewer disposition per cycle, not per
feature. Default is reviewer-on; flip to skip only when the table below
says the reviewer cannot add signal.

| Cycle shape | Reviewer disposition | Why |
|-------------|----------------------|-----|
| Architectural / handoff cycles (e.g. plan 215 M5, plan 179 M3 patterns) | Load-bearing — always dispatch | Cross-cutting choices need a fresh, independent read against the spec. |
| Schema migrations | Load-bearing — always dispatch | Forward + back safety, FK implications, index correctness all require a second pair of eyes. |
| New CLI surfaces | Load-bearing — always dispatch | Flag semantics, error messages, `--json` shape, exit codes harden against an independent read. |
| Validate / invariant changes | Load-bearing — always dispatch | Each rule must be covered by a test and the reject paths must return useful errors. |
| Refactor sweeps with semantic implications | Load-bearing — always dispatch | Not pure mechanical — behavior may shift under the rename. |
| Single-feature additions (the middle ground) | Default reviewer-on; flip to skip only if the diff is small and non-architectural | Treat the table edges as the bright lines; the middle defaults to on. |
| Decision-only cycles (recording `planar decision add` rows) | Skip — reviewer adds no signal | Reviewer cannot verify decision content beyond what the operator already approved. |
| Pure mechanical sweeps (mass `sed`-style refactors, e.g. plan 162 M7 pl-adopt → pl-import across 73 files) | Skip — reviewer adds no signal | The diff IS the verification; grep-verified completeness suffices. |
| Docs-polish without behavior change | Skip — reviewer adds no signal | Voice review is a different job; render-check + link checks cover the load-bearing parts. |

When the reviewer IS dispatched, see the role spec at
[`agents/reviewer.md`](reviewer.md) for the focused responsibilities and
the explicit NOT-do list. The blind-read contract below governs how the
brief is composed.

### Blind-read contract

The reviewer brief MUST NOT include the coder's report. A reviewer who
reads the coder's narrative inherits the coder's framing — the exact
framing the coder may have rationalized past. The reviewer sees only:
the task IDs the coder claimed, the relevant spec/roadmap section paths
(not bodies — the reviewer reads the files independently), a directive
to run `git diff HEAD` and `git diff --stat HEAD` themselves, and the
quality-gate output (test count, integration confirmation) since the
reviewer is not re-running gates. The orchestrator preserves this
contract: when dispatching the reviewer, it composes a fresh brief
rather than pasting the coder's full report.

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
  list. "Implement M3" is not a scope; "tasks 1247, 1248, 1249 under
  plan 227 M3" is. Task IDs are the contract the reviewer compares the
  diff against.
- **List claim tokens explicitly.** The cycle's synchronization scope is
  the claim token set. The reviewer uses it to verify the diff stayed
  inside leased work, and an interrupted session uses it to resume or
  reconcile stale ownership.
- **Note locked decisions inline.** Patterns like `Q47 = editor markers`
  or "ADR-0012 forbids new global state here" go in the brief verbatim,
  so the coder does not re-litigate them. A decision restated in the
  brief is faster to honor than one buried in an ADR the coder may not
  reach.
- **Specify the gates the coder must run.** Default Go gates: `gofmt`,
  `go vet`, `go build`, `go test`, plus the two-run integration
  confirmation, `make render-check` when surfaces are touched, and any
  remaining relevant validators.
  Naming the gates in the brief means the coder cannot omit them as
  "obvious."
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
  `verifies` edges to test-spec scenarios (plan 277).** The test-spec
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
| `approve`          | Output meets the task acceptance signal. On iteration 5 specifically, may be approved with caveats — those caveats are recorded as decisions or follow-up tasks. | Orchestrator marks task done and proceeds to the next dispatch. |
| `request-changes`  | Output is salvageable but needs specific edits. Reviewer attaches concrete remediation. Not valid on iteration 5. | Orchestrator returns to coder for the next iteration. Iteration counter increments. |
| `open-question`    | Reviewer cannot reconcile a question without user input (ambiguous spec, scope conflict, missing decision). The reviewer must first attempt reconciliation against the spec, ADRs, and prior decisions; this decision is reserved for genuine blockers. | Orchestrator pauses the task and surfaces the question to the user. The task resumes when the user answers. |
| `abort`            | The task as currently scoped should not proceed. Reviewer documents why. | Orchestrator escalates to the user with the abort reason and WIP state. The task does not resume without user intervention. |

## Escalation

1. **Open question (any iteration).** The reviewer must first attempt reconciliation against the spec, ADRs, and prior decisions. Only when reconciliation genuinely fails does the question become an `open-question` decision routed through the orchestrator to the user.
2. **Iteration 5 reached without `approve` on iterations 1–4.** The reviewer chooses `approve` with caveats or `abort`. Either way, the orchestrator produces a user-facing summary: what was attempted, what blocked acceptance, and the recommended next step.
3. **Abort (any iteration).** The orchestrator preserves the WIP state and surfaces it to the user. The task does not resume without user intervention.

## Concurrency

- The orchestrator may dispatch multiple coders in parallel only when tasks are independent and claimable: disjoint file scope, no shared schema or CLI surface changes pending, no decision dependency between them, and no active unexpired claim already owns the same task or child milestone.
- Reviewers may run in parallel against independent coder outputs.
- A single task is always coder→reviewer sequential — never two coders on the same task simultaneously.
- Stale claims are not ignored silently. The operator or orchestrator must reconcile or force-takeover them before treating the work as available.

## State Capture

State capture lives in SQLite per the locked schema. Tasks carry `next_action`. `sessions` and `session_entries` capture the iteration timeline. `agent_work_claims` records live ownership and lease state. `agent_actions` records typed time-bounded work inside sessions. `decisions` records reviewer rulings. Open questions are `question` rows linked to the task. Aborts surface as session entries with `prefix='error'` linked to the task and the originating decision, and the corresponding claim is released as `aborted` or later reconciled as `stale`.

## Things To Revisit

- **Iteration cap of 5.** Hard-coded today. Revisit once empirical data on real workloads exists — the right number may be 3, 5, or 8 depending on task shape. Move to the `config` table or methodology frontmatter if it needs to flex per project.
- **Per-tier reviewer.** Currently all reviewers are `large`-tier. Some review work may not need that; a cheaper review tier could be useful for routine tasks.
- **Parallelism heuristics.** "Independent task" is judgment-call territory today. Codify which task shapes are safely parallel as patterns emerge.
- **Cross-vendor pairings.** A reviewer may be a different vendor than the coder (Claude reviewing Codex output, etc.). The methodology assumes this works; verify once cross-vendor handoff is exercised in M6.
- **Reviewer feedback format.** "Concrete remediation" is loose today. As patterns emerge, codify the structure (e.g. file:line + proposed change, or a structured issue list).
