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

The claim ritual is expressed entirely in `planar-agent` verbs (the dedicated agent-side binary; there is **no** `planar agent` subcommand on the `planar` binary). The canonical sequence is **pull → heartbeat → terminate** with `complete` / `fail` / `release` / `block`:

1. **Check.** Before selecting work, the orchestrator runs `planar plan next <plan>` (operator-side claim-aware read) or `planar-agent peek <plan>` (agent-side dry-run of "what would `pull` pick"). Do not derive next work from `task list --status todo` alone.
2. **Pull.** `planar-agent pull <plan-id> [--role coder] [--worktree <id-or-path>]` atomically picks the next eligible task, claims it (`agent_work_claims.status='active'`), flips the task to `doing`, and starts a top-level `agent_actions` row. Returns `{claim_token, task, action_id}`. When no work is available it returns `{ok:true, no_work:true}` and the agent terminates cleanly.
   For dispatch where the caller already has a specific task ID (orchestrator hand-picking), use `planar-agent claim --entity task:<id> [--role <r>] [--worktree <path>]` instead. `claim` does not auto-transition the task; the caller is responsible for the status flip (or for invoking `planar-agent action start` to mark work as begun without touching task status).
3. **Heartbeat.** `planar-agent heartbeat --claim <token> [--ttl <secs>]` at least once per TTL/2 while work continues. A long-running tool call may delay the heartbeat, but the agent should heartbeat immediately before and after such calls.
4. **Report sub-actions (optional).** For granular telemetry, wrap tool calls in `planar-agent action start --claim <token> --kind tool_call` / `planar-agent action end --action <id> --outcome ok`. Most agents skip this and let the top-level action started by `pull` cover the whole work session.
5. **Terminate** with exactly one of:
   - `planar-agent complete --claim <token> [--summary <text>]` — work succeeded; task → `done`, claim → `completed`.
   - `planar-agent fail --claim <token> --reason <text>` — work failed; task back to `todo`, claim → `aborted`.
   - `planar-agent release --claim <token> [--reason <text>]` — graceful give-up without attempting; task back to `todo`, claim → `released`.
   - `planar-agent block --claim <token> --blocker <task-id> [--reason <text>]` — hit an external blocker; task → `blocked`, blocker edge created, claim → `released`.

   If the process dies without invoking any of these, `planar-agent reconcile` (operator-side, separate binary) later marks the lease stale. The task stays in `doing` until the operator revives it or another agent's `pull` finds it available again (a stale claim no longer blocks pull's exclusivity).
6. **Operator recovery.** Stuck claims and the human cleanup path live on `planar-agent`, not `planar`: `planar-agent reconcile [--dry-run] [--stale-after <secs>]` for batch stale-claim sweeps, and `planar-agent abort --claim <token> --reason <text>` to force-release a specific claim regardless of the owning session.
7. **Live observability.** The read-only `planar-watch` binary (plan 85 M8) carries `ps`, `feed --follow`, `log`, `claims`, `actions`, and `plans` for streaming human observation. For non-streaming reads through the `planar` binary, use `planar dashboard --agents`, `planar plan next`, `planar tree`, `planar audit trail`, and `planar health`.

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

## Orchestration strategies

An orchestration strategy is a named bundle of the five underlying dispatch axes the orchestrator uses to drive a plan: `isolation`, `branch_model`, `concurrency`, `reviewer_cadence`, and `test_coder_cadence`. Strategy is the operator-facing dispatch frame; the existing dispatch-shape gate (strict / grouped / single / barrel-*) runs **nested** under the strategy choice, not parallel to it. A strategy answers "what is the overall methodology for this plan?" The dispatch shape then answers "within that strategy, how do I batch *this cycle's* work?"

Storage is path-of-least-resistance: **no schema delta.** The strategy choice for each cycle lives in the existing `agent_actions.metadata` JSON column on the dispatch row; the orchestrator derives the "last-used strategy for this plan" by reading the most recent dispatch entry's metadata. New plans default to `classic`.

### Named strategies

Five named strategies ship. Each is one row in the bundle table below; an operator who wants something outside the menu can assemble a custom combination via axis-by-axis flags (see [Strategy gate](#strategy-gate) below).

- **`classic`** — Coder runs in the operator's cwd on whatever branch is currently checked out. Sequential cycles, reviewer per cycle, test-coder per cycle. No worktrees, no epic branch, no parallelism. Recommended for: single-task changes, small plans, high-stakes invariant-touching work where the operator wants to watch the diff land in their own checkout.
- **`isolated-sequential`** — Coder runs in a dedicated worktree on a child branch off an epic branch. Sequential cycles, reviewer per cycle. The operator's pwd stays clean; recovery from a dead coder is straightforward; `git log --graph` shows per-task working branches. Recommended for: multi-task plans where pwd hygiene matters, cross-cutting work, plans where easy per-task rollback is valuable.
- **`parallel-fanout`** — Fan out to N parallel coders on a parallel-eligible subset of the plan's open tasks, consolidate at fan-in, single reviewer pass per fan-in cycle. Each coder runs in its own worktree on its own child branch. Recommended for: plans with ≥3 tasks and ≥2 parallel-eligible. Refused for: schema-migration-heavy plans, singleton-file-editing plans (the parallelizability rules surface this automatically — see the forward pointer in [Things To Revisit](#things-to-revisit)).
- **`barrel-deferred`** — Coder cycles run back-to-back in pwd; reviewer dispatched once at a milestone or plan boundary on the union diff. No isolation. Recommended for: long sequential plans where per-cycle reviewer overhead exceeds the value. This is the existing `barrel-deferred` dispatch shape promoted to a named strategy.
- **`barrel-bypass`** — No reviewer at all; quality gates (`make fmt-check`, `make build`, `make test`, `make test-integration` twice, `planar skills render --check` against an out-of-tree staging dir, and any remaining relevant validators) are the entire signal. Sequential, in-pwd. Recommended for: mechanical sweeps, docs-polish, single-verb additions where the contract is fully gated.

### Continuity guarantee: `classic`

`classic` is the explicit continuity default, not a legacy or deprecated mode. It matches today's operator behavior bit-for-bit: coder in pwd, current branch, sequential cycles, reviewer per cycle. An operator who picks (or accepts the recommendation of) `classic` sees no behavioral change relative to today — no worktree is created, no epic branch is cut, no parallel dispatch happens. This is a first-class supported strategy and a design promise: introducing the strategy menu must not require existing operators to learn a new flow to keep working as they do.

### Axes

The five axes underlying every strategy. A custom strategy (`--strategy custom` with per-axis flags) is the escape hatch for advanced operators outside the named menu.

| Axis | Values | `classic` default |
|------|--------|-------------------|
| `isolation` | `in-pwd`, `worktree` | `in-pwd` |
| `branch_model` | `current-branch`, `epic-child` | `current-branch` |
| `concurrency` | `sequential`, `fan-out` | `sequential` |
| `reviewer_cadence` | `per-cycle`, `per-fanin`, `at-boundary`, `gates-only` | `per-cycle` |
| `test_coder_cadence` | `per-cycle`, `per-fanin`, `at-boundary`, `none` | `per-cycle` |

### Named bundles

Each strategy locks in one value per axis:

| Strategy | `isolation` | `branch_model` | `concurrency` | `reviewer_cadence` | `test_coder_cadence` |
|----------|-------------|----------------|---------------|--------------------|----------------------|
| `classic`             | in-pwd   | current-branch | sequential | per-cycle    | per-cycle    |
| `isolated-sequential` | worktree | epic-child     | sequential | per-cycle    | per-cycle    |
| `parallel-fanout`     | worktree | epic-child     | fan-out    | per-fanin    | per-fanin    |
| `barrel-deferred`     | in-pwd   | current-branch | sequential | at-boundary  | at-boundary  |
| `barrel-bypass`       | in-pwd   | current-branch | sequential | gates-only   | none         |

### Invalid combinations

The orchestrator refuses these axis combinations with a diagnostic before any dispatch runs:

- `concurrency=fan-out` with `isolation=in-pwd` — parallel coders would clobber pwd.
- `concurrency=fan-out` with `branch_model=current-branch` — no fan-in target.
- `reviewer_cadence=per-cycle` with `concurrency=fan-out` — contradicts the fan-out reviewer model; reviewer-per-child is a separate decision tracked as an open question.

### Recommendation algorithm

The orchestrator proposes a strategy per plan based on plan shape, with status-quo bias. The algorithm runs in the orchestrator skill, sourcing the inputs it needs from existing CLI reads (`planar plan show --json`, `planar task list --json`, `planar audit trail`). If the in-skill implementation drifts, an engine-side `planar plan recommend-strategy --json` flag becomes the parallel of the deferred `--parallel-eligible` flag.

1. If the plan is tagged as mechanical, docs-only, or single-verb → recommend `barrel-bypass`.
2. Else if the plan has a multi-milestone roadmap and at most one parallel-eligible task per milestone → recommend `barrel-deferred`.
3. Else if the plan has ≥3 tasks and the parallel-eligible subset has ≥2 tasks → recommend `parallel-fanout` (with the eligible subset surfaced for confirmation).
4. Else if the plan has 2–3 tasks and none are parallel-eligible → recommend `isolated-sequential`.
5. Else if the plan has exactly 1 task → recommend `classic`.
6. Else if the most recent dispatch on this plan used a non-default strategy `S` → recommend `S` (stickiness — the operator already made a choice for this plan).
7. Otherwise → recommend `classic` (status-quo bias).

The recommendation is a proposal, never an action. The strategy gate (below) is what turns it into a chosen strategy.

### Strategy gate

Phase 3 of the orchestrator now runs **two** gates in order before dispatch:

1. **Strategy gate** (new). "Which strategy for this plan?" The orchestrator surfaces its recommended strategy + a one-line rationale + the named alternatives. The operator confirms or overrides.
2. **Dispatch-shape gate** (existing — see [Dispatch Granularity](#dispatch-granularity)). "Within that strategy, which shape for this cycle?" Constrained by the strategy: `parallel-fanout` forces the `fan-out` shape; `barrel-bypass` forces the `barrel-bypass` shape; `barrel-deferred` forces the `barrel-deferred` shape; `classic` and `isolated-sequential` keep the full strict / grouped / single menu.
3. **Dispatch.** Orchestrator creates worktrees and branches (under strategies with `isolation=worktree`), claims tasks, dispatches coders.

The strategy gate is operator-confirmed by default. Skip flags:

- `--strategy <name>` — pre-commit to a named strategy. Skips the gate; the dispatch-shape gate still runs (unless that gate also has a pre-committed answer).
- `--strategy custom --isolation <X> --branch-model <Y> --concurrency <Z> --reviewer-cadence <W> --test-coder-cadence <V>` — pre-commit to a custom axis combination. The per-axis flags are hidden from default `--help`; advanced operators discover them via docs or `--help-advanced`.

Auto-defaulting without confirmation is **not** a supported mode — the recommendation engine never silently picks a strategy. If the operator wants zero-friction repetition, `--strategy <name>` is the explicit opt-in.

**Persistence.** No new schema. The orchestrator writes the chosen strategy into the dispatch entry's `agent_actions.metadata` JSON column on the dispatch row:

```json
{
  "strategy": "isolated-sequential",
  "axes": {"isolation": "worktree", "branch_model": "epic-child", "concurrency": "sequential", "reviewer_cadence": "per-cycle", "test_coder_cadence": "per-cycle"},
  "dispatch_shape": "strict",
  "rationale": "2-task plan, neither parallel-eligible"
}
```

The "last-used strategy for this plan" lookup that drives rule 6 of the recommendation algorithm is a single indexed read against the most recent dispatch entry's metadata for the plan. No `plans.strategy` column, no separate `plan_strategies` table — operational state belongs to the dispatch row that recorded the choice.

### Relation to existing dispatch shapes

The dispatch-shape gate's six shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`) map onto the strategy axes as follows:

- `strict`, `grouped`, `single` — apply within any strategy. They describe per-cycle batching, not overall methodology. Under `classic` or `isolated-sequential` the operator picks freely; under `parallel-fanout` they describe how each fan-out lane is batched; under the barrel strategies they are subsumed.
- `barrel-grouped`, `barrel-deferred`, `barrel-bypass` — these conflate "dispatch shape" with "reviewer cadence." Under the strategy model, the latter two are subsumed by the `barrel-deferred` and `barrel-bypass` named strategies. Whether the standalone flags get deprecated, kept for backward compatibility, or treated as aliases is an open question deferred until operators have used both paths for a cycle or two.

## Worktrees

Worktrees are the substrate that lets `isolated-sequential` and `parallel-fanout` keep the operator's pwd clean and (under fan-out) run N coders concurrently without clobbering each other. `classic` and the barrel strategies do not use them. The methodology in this section is **authoritative**; the tech-spec artifact for plan 297 (artifact 142) still carries the original bare-`<plan-slug>` convention in its prose and will be amended to match in a follow-up.

### Path scheme

Two worktree shapes, both rooted at the task's owning repo:

| Worktree | Path | Created by |
|----------|------|------------|
| Epic (integration) | `<repo>/.worktrees/epic/<plan-slug>/` | Orchestrator, on the first dispatch of any task in the plan. |
| Cycle (per-cycle working tree) | `<repo>/.worktrees/cycle/<plan-slug>/<task-slug>/` | Orchestrator, on each cycle dispatch. |

`<repo>` is the *task's owning repo*, not the operator's cwd repo. In a polyrepo workspace these may differ; the worktree always lands under the task's repo so the orchestrator's `git` invocations resolve correctly against the right history.

### Branch scheme

Epic and cycle branches live in **disjoint top-level namespaces** so git's ref hierarchy never refuses a nested form:

| Branch | Name | Cut from |
|--------|------|----------|
| Epic (integration) | `epic/<plan-slug>` | master |
| Cycle (per-task working branch) | `cycle/<plan-slug>/<task-slug>` | `epic/<plan-slug>` |

The original spec's bare `<plan-slug>` + `<plan-slug>/<task-slug>` pairing collides: git refuses any ref whose path is a strict prefix of another existing ref (e.g., `worktree-management` cannot coexist with `worktree-management/m2-worktree-convention`). The `epic/` and `cycle/` prefixes guarantee no ref-hierarchy collision. This supersedes the older convention in artifact 142.

Hand-picked solo work (operator runs `planar-agent claim --entity task:<id>` directly with no plan context) uses a single branch `cycle/<task-slug>` cut from master, with no epic. The worktree lands at `<repo>/.worktrees/cycle/<task-slug>/`.

### Topology and main-checkout invariant

The operator's **main checkout stays on master throughout the entire orchestration.** Both the epic branch and each cycle's child branch live in their own worktrees off the main checkout. There is never a moment when the main checkout silently moves to an epic or cycle branch.

- Main checkout: `<repo>/` — always on master, untouched by orchestration.
- Epic worktree: `<repo>/.worktrees/epic/<plan-slug>/` — checked out on `epic/<plan-slug>`. Created on first dispatch, persists for the plan's duration.
- Cycle worktree: `<repo>/.worktrees/cycle/<plan-slug>/<task-slug>/` — checked out on `cycle/<plan-slug>/<task-slug>`. One per cycle.

**Fan-in.** The orchestrator cds into the *epic* worktree and runs `git merge --no-ff cycle/<plan-slug>/<task-slug>` there. The main checkout is not involved. This preserves the "operator pwd stays clean" promise that motivates the worktree strategies in the first place.

### Lifecycle

1. **Epic worktree creation** (first dispatch of any task in the plan only). `git -C <repo> branch epic/<plan-slug> master` then `git -C <repo> worktree add <repo>/.worktrees/epic/<plan-slug>/ epic/<plan-slug>`. If `.git/info/exclude` doesn't list `.worktrees/` yet, append it.
2. **Cycle worktree creation** (each cycle). `git -C <repo> worktree add -b cycle/<plan-slug>/<task-slug> <repo>/.worktrees/cycle/<plan-slug>/<task-slug>/ epic/<plan-slug>`.
3. **Claim wiring.** `planar-agent pull <plan> --role coder --worktree <cycle-worktree-path>` (or `claim --entity task:<id> --worktree <path>` for hand-picked). The claim row carries `worktree_path`.
4. **Coder dispatch.** Agent invoked with `isolation: "worktree"` and the cycle worktree path. Coder heartbeats and terminals via the canonical `planar-agent` ritual.
5. **Fan-in merge.** After terminal-complete the orchestrator cds into the epic worktree and runs `git merge --no-ff cycle/<plan-slug>/<task-slug>`. Clean merge → continue. Conflict → open a question, halt this branch's fan-in, leave the cycle worktree in place for operator resolution.
6. **Reviewer dispatch.** Per the active strategy: per-cycle under `isolated-sequential`; once per fan-in under `parallel-fanout`.
7. **Cleanup (post-success only).** After reviewer approval: `git -C <repo> worktree remove <cycle-worktree-path>` and `git -C <repo> branch -D cycle/<plan-slug>/<task-slug>` (force delete because cycle branches are merged into epic but not into master HEAD). The epic worktree persists.
8. **Epic merge to master.** Operator-driven. Orchestrator surfaces "`epic/<plan-slug>` is N commits ahead of master, reviewer-approved, ready for PR." After the epic→master merge lands, the epic worktree and `epic/<plan-slug>` branch are removed.

Cleanup is **post-success only** — never before merge + reviewer approval, so a failed cycle leaves recoverable artifacts on disk.

### `.git/info/exclude` hint

`.worktrees/` is appended to `.git/info/exclude` on first use per clone (per-clone, no commit needed). Operators working in shared clones may prefer to commit a repo-level `.gitignore` entry instead; either path is fine. The orchestrator does the per-clone append on first dispatch; the gitignore commit is an operator choice.

### Sparse-checkout guidance for monorepos

For repos > ~5 GiB the operator should configure `git sparse-checkout set <paths>` on the source clone before letting the orchestrator dispatch into a worktree. Worktrees inherit sparse-checkout settings via the shared `.git/config`. The orchestrator does not measure repo size and does not prompt for sparse setup — it's a documented operator practice, not an automated flow. The recipe lives in `docs/workflows.md`.

### Scope inside worktrees — parent repo dictates, planning verbs refused

Two invariants govern scope behavior when cwd is inside a worktree:

1. **The parent repo always dictates the scope.** A worktree at `<repo>/.worktrees/{epic,cycle}/...` resolves to the same association as `<repo>`. Worktrees are not separately scoped entities; they inherit. Reads (`planar plan list`, `planar task show`, `planar-watch *`) work transparently from inside a worktree and see the parent's data.
2. **Planning verbs are refused from inside worktrees.** The runtime entry point refuses the planning-class verb set with a distinct exit code and a message pointing at the parent repo's cwd.

**Planning-class verbs (refused in worktree):** `init`, `plan {add,update,done}`, `task {add,update,done,touches}`, `question {add,answer,wontfix}`, `decision {add,accept,reject}`, `artifact {add,update}`, `scenario {add,update}`, `spec {draft,ingest}`, `link`, `unlink`, `links {add,remove}`, `assoc {add,update}`, `promote`, `demote`.

**Execution / read (allowed in worktree):** every `planar-agent *`, every `planar-watch *`, `planar resume`, `planar dashboard`, `planar handoff *`, `planar capture *`, `planar audit *`, `planar health`, every `* show` / `* list` read, `planar workbench {pull,push,status,sync,resolve}`, `planar workspace *`.

**`task done` is refused on purpose.** Coders advance task state via `planar-agent complete --claim <token>`, the atomic terminal verb that flips claim status and task status in one transaction. This reinforces the three-binary boundary in `CLAUDE.md § Operating Rules` and the canonical claim ritual in [Coordination claims](#coordination-claims).

**`--scope <slug>` does NOT override the refusal.** The rule is about *where the verb runs*, not which scope it targets. To plan against a member repo from elsewhere, cd to the parent repo (or workspace root with `--scope <member>`); do not try to plan from inside a worktree.

The engine-side enforcement of this rule lands in M3 of plan 297 (scope resolver extension + verb-classification table at the runtime entry point). Until M3 lands, the rule is convention-only; the methodology defines it so the engine implementation has a target.

### Parallelizability rules

Used by the `parallel-fanout` strategy's eligibility test and by the per-cycle fan-out width within that strategy. Two tasks in the same plan are **parallel-eligible** iff *all* six rules hold:

1. **No `blocked_by` chain to another not-yet-done task in the plan.** The task's transitive `blocked_by` closure intersected with the plan's not-done tasks must be empty.
2. **Disjoint `task_touches`.** The candidates' `task_touches` (repo + path) sets must not overlap. Empty `task_touches` is treated as "touches everything" → not parallel-eligible. This deliberately forces touches discipline before fan-out becomes available.
3. **No schema migration touched.** A task whose touches include `migrations/*.sql` serializes across the whole plan. Planar's migration numbering is linear; two concurrent migrations would collide on the next number.
4. **No singleton authoritative file touched.** Touches must not include any of: `agents/methodology.md`, `CLAUDE.md`, `AGENTS.md`, `docs/cli-reference.md`, `docs/architecture.md`. These are coordination points; concurrent edits invite needless conflicts. The list lives here and may grow.
5. **No unresolved open question linked to the task.** Open questions are coordination points by definition.
6. **No unresolved decision dependency.** A task blocked on a `decisions` row in `proposed` status does not fan out.

The algorithm is greedy: start with the full open-task set, drop ineligible tasks rule-by-rule, return what remains. Ties (e.g., two tasks both touching a migration) drop both, not just one — the orchestrator surfaces both as "serialize-only" with the rule that excluded them. A result of size ≥ 2 makes fan-out an available shape under [`parallel-fanout`](#named-strategies); smaller results fall back to the strategy's sequential shapes (strict / grouped / single).

The forward reference from [Things To Revisit → Parallelism heuristics](#things-to-revisit) now resolves to these six rules.

### Conflict resolution at fan-in

`git merge --no-ff cycle/<plan-slug>/<task-slug>` from inside the epic worktree may fail. The orchestrator's response:

- **Touches mis-prediction.** Tasks claimed disjoint paths but both modified the same file (e.g., a transitive import dragged in a shared header). Orchestrator opens a question with the conflict-marker output, halts that branch's fan-in, leaves the cycle worktree on disk for operator resolution.
- **Trivial conflict** (adjacent additions, import order, formatter idempotency disagreements). Same path: open a question. The orchestrator does not classify "trivial vs. semantic" — that's an operator judgement call.
- **Deep semantic conflict** (two children each broke an invariant the other relies on). Open question. Operator decides whether to (a) resolve manually and continue, (b) abort the parallel epic and re-dispatch sequentially, or (c) revert one child and redo it after the other lands.

In every case the orchestrator preserves recoverable state: the cycle worktree stays, the cycle branch stays, the claim records reflect the partial fan-in. Cleanup only runs once the conflict is resolved and the cycle is merge-clean.

## Dispatch mode selection

Use INLINE (skip reviewer dispatch) if ALL of the following hold:
  (a) ≤3 files modified
  (b) Only mechanical edits: renames, deletions, mass symbol-replace,
      comment-only changes, or whitespace normalization
  (c) Automated validation green pre-submit: `make fmt-check` clean,
      `make build` clean, `make test` green (or scoped to affected
      modules), `make test-integration` green (run twice), project
      grep checks pass, `planar skills render --check` against an
      out-of-tree staging dir and any remaining relevant validators pass

Use STRICT (full coder + reviewer dispatch) if ANY of the following hold:
  - More than 3 files modified
  - Logic changes, new functions, new types, or new DB access
  - Spec, ADR, or migration changes
  - Reviewer failed the previous cycle on the same area

## Common defects pre-flight checklist

Run this checklist before every work-complete declaration. Add new entries
when a cycle surfaces a defect not already listed.

- [ ] `make fmt-check` returns clean (all Zig files format-clean).
      Note: run this AFTER any bulk substitution (`sed`, find-replace).
      `zig fmt` does not run automatically on substituted files.
- [ ] For any reference to a schema enum value (e.g. artifact kind, task
      status, plan status): confirm the value by reading the migration SQL
      (`migrations/`) — never guess from memory.
- [ ] For any documentation table (especially in ADRs or tech-specs):
      cross-check each row against the authoritative source. Tables that
      summarize values from other documents must be validated against those
      documents, not just internally consistent.
- [ ] `make fmt-check` returns no diagnostics.
- [ ] `make build` succeeds.
- [ ] `make test` green (or scoped to modified packages).
- [ ] `make test-integration` green (run **twice** back-to-back).
- [ ] `planar skills render --check` against an out-of-tree staging dir passes if any skill/agent surface was modified.
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

No reviewer dispatch at all. Coder cycles run back-to-back; quality gates (`make fmt-check`, `make build`, `make test`, `make test-integration` twice, `planar skills render --check` against an out-of-tree staging dir, plus any remaining relevant validators) are the entire signal. The iteration cap is *undefined* — there is no reviewer, so no `request-changes`, so no iteration.

The contract under `barrel-bypass`: the coder's quality-gate output IS the review. The coder must paste gate citations verbatim in the work-complete report, run the integration suite twice, and (if vendor/agent surfaces are touched) run `planar skills render --check` against an out-of-tree staging dir and any remaining relevant validators. A coder dispatched under `barrel-bypass` must know its situation; the coder agent spec acknowledges this contract explicitly.

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
- **Specify the gates the coder must run.** Default Zig gates:
  `make fmt-check`, `make build`, `make test`, plus the two-run
  `make test-integration` confirmation, `planar skills render --check`
  against an out-of-tree staging dir when surfaces are touched, and any
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

- The orchestrator may dispatch multiple coders in parallel only when tasks are independent and claimable: disjoint file scope, no shared schema or CLI surface changes pending, no decision dependency between them, and no active unexpired claim already owns the same task or child milestone. The codified eligibility test is the six rules in [Worktrees § Parallelizability rules](#parallelizability-rules); the [`parallel-fanout`](#named-strategies) strategy is the orchestration frame that uses them.
- The substrate that lets parallel coders run without clobbering each other is [Worktrees](#worktrees) — each parallel coder runs in its own cycle worktree on its own `cycle/<plan-slug>/<task-slug>` branch, with fan-in merging back onto the plan's epic worktree.
- Reviewers may run in parallel against independent coder outputs. Under `parallel-fanout` a single reviewer cycle runs against the integrated diff on the epic, not per child.
- A single task is always coder→reviewer sequential — never two coders on the same task simultaneously.
- Stale claims are not ignored silently. The operator or orchestrator must reconcile or force-takeover them before treating the work as available.

## State Capture

State capture lives in SQLite per the locked schema. Tasks carry `next_action`. `sessions` and `session_entries` capture the iteration timeline. `agent_work_claims` records live ownership and lease state. `agent_actions` records typed time-bounded work inside sessions. `decisions` records reviewer rulings. Open questions are `question` rows linked to the task. Aborts surface as session entries with `prefix='error'` linked to the task and the originating decision, and the corresponding claim is released as `aborted` or later reconciled as `stale`.

## Things To Revisit

- **Iteration cap of 5.** Hard-coded today. Revisit once empirical data on real workloads exists — the right number may be 3, 5, or 8 depending on task shape. Move to the `config` table or methodology frontmatter if it needs to flex per project.
- **Per-tier reviewer.** Currently all reviewers are `large`-tier. Some review work may not need that; a cheaper review tier could be useful for routine tasks.
- **Parallelism heuristics.** The six rules in [Worktrees § Parallelizability rules](#parallelizability-rules) codify what was previously judgment-call territory. They are deliberately conservative — empty `task_touches` is treated as "touches everything," singleton authoritative files block fan-out, schema migrations serialize. The list of singleton files may grow as new coordination points emerge; revisit once enough fan-out cycles have shipped to identify whether the conservatism has the right shape.
- **Cross-vendor pairings.** A reviewer may be a different vendor than the coder (Claude reviewing Codex output, etc.). The methodology assumes this works; verify once cross-vendor handoff is exercised in M6.
- **Reviewer feedback format.** "Concrete remediation" is loose today. As patterns emerge, codify the structure (e.g. file:line + proposed change, or a structured issue list).
