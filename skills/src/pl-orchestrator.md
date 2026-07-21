---
slug: pl-orchestrator
description: "Run the orchestrator over a goal, anchor plan, or task list — manage the full feature lifecycle (planning, ingestion, execution, finalization, propagation, archive) with reviewer iteration cap and user gates at each phase boundary."
source: agents/orchestrator.md
cross_scope_writes: true
model_tier: large
vendor:
  claude:
    argument_hint: "<goal|plan-id|task-id> [<task-id>...] [--finalize] [--propagate] [--archive] [--strategy <name>] [--isolation <pwd|worktree>] [--no-docs] [--strict | --grouped | --batch <ids>]"
    invocation_examples: |
      /orchestrator <goal>                          # start from scratch: plan → wait → ingest → wait → execute
      /orchestrator <anchor-plan-id>                # resume from current anchor plan status
      /orchestrator <task-id> [<task-id>...]        # execute specific tasks (Phase 3 only)
      /orchestrator <goal> --propagate              # plan → ingest → execute → propagate
      /orchestrator <anchor-plan-id> --finalize     # execute → dispatch janitor → merge + reconcile + closeout
      /orchestrator <anchor-plan-id> --finalize --archive  # execute → finalize → archive FS tree
      /orchestrator <anchor-plan-id> --archive      # execute → mark done → archive FS tree
      /orchestrator <plan-id> --strategy classic              # explicit continuity — coder in pwd, current branch, sequential
      /orchestrator <plan-id> --strategy classic --isolation worktree  # sequential cycle worktrees; reviewer per cycle
      /orchestrator <plan-id> --strategy barrel-deferred      # back-to-back coder cycles in pwd; reviewer at boundary
      /orchestrator <plan-id> --strategy barrel-deferred --isolation worktree  # back-to-back cycle worktrees; reviewer at boundary
      /orchestrator <plan-id> --strategy barrel-bypass        # no reviewer; gates are the entire signal
      /orchestrator <plan-id> --strict              # one coder cycle per task (skip the dispatch-shape gate)
      /orchestrator <plan-id> --grouped             # orchestrator picks groupings (skip the dispatch-shape gate)
      /orchestrator <plan-id> --batch 8,9,10 --batch 11,12  # explicit grouping; repeatable (skip the dispatch-shape gate)
      /orchestrator <plan-id> --barrel-grouped      # DEPRECATED alias for --grouped; prefer --strategy barrel-deferred
      /orchestrator <plan-id> --barrel-deferred [--barrel-deferred-at milestone|plan]  # DEPRECATED; prefer --strategy barrel-deferred
      /orchestrator <plan-id> --barrel-bypass       # DEPRECATED; prefer --strategy barrel-bypass
shared_notes:
  - "Dispatches to the host vendor's Planar skill surfaces by default; cross-vendor dispatch uses the destination vendor's invocation surface."
---

# Orchestrator ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `orchestrator` agent. See [`agents/orchestrator.md`](../../agents/orchestrator.md) for the full role spec, phase descriptions, and phase-selection logic. See [`agents/methodology.md`](../../agents/methodology.md) for the iteration cap, reviewer decisions, concurrency rules, and escalation paths.

## Isolation invariant (read this before dispatching any coder)

**The orchestrator loop never edits repository files.** It only runs `planar` / `planar-agent` CLI verbs and spawns / collects subagents. Every working-tree mutation must happen inside a freshly spawned coder subagent with blank context.

**"Dispatch to a coder" means spawning a subagent, not invoking `/pl-coder` inline.** A slash command runs in the caller's own context and model — that collapses the orchestrator and coder into a single agent, which is the defect this rule prevents. Spawn a fresh subagent through the host's subagent dispatch surface.

This is Axis A of the two-axis dispatch model and it is non-negotiable. It is independent of:
- **File count or edit triviality.** Even a one-line doc fix must go through a spawned coder.
- **Model tier.** Even when the orchestrator is already running at `large` (opus), the coder must be a separate spawned subagent. "I'm already the best model, spawning adds nothing" is a rationalization that violates isolation.

**Axis B (reviewer disposition)** is the tunable axis: whether a reviewer pass runs after the coder. The six dispatch shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`) and the reviewer-dispatch profile are Axis B — they govern reviewer behavior, not coder isolation.

## Phase Behavior

The orchestrator selects phases based on the anchor plan's current `status`:

1. **Planning (Phase 1)** — anchor plan in `draft` with no workbench artifacts: invokes `pl-spec-draft "<goal>"`, surfaces the drafted artifacts to the user, and **waits for explicit review** before proceeding. Does not auto-advance.

2. **Ingestion (Phase 2)** — anchor plan in `draft` with workbench artifacts present: invokes `pl-spec-ingest <plan>` in preview mode (no `--apply`), presents the diff to the user, and **waits for explicit confirmation** before running `--apply`. Never auto-applies.

3. **Execution (Phase 3)** — anchor plan `active` or `paused`: first reads claim-aware state with `planar plan next <plan>` (operator-side) or `planar-agent peek <plan>` (agent-side dry-run for explicit task IDs), excludes active unexpired claims, and surfaces stale claims before dispatch. Phase 3 then runs **two gates in order** before any coder runs: the **strategy + isolation gate** (new — see below), which picks the overall methodology for the plan (`classic` / `barrel-deferred` / `barrel-bypass` / `parallel-fanout`) and, for sequential strategies, whether it runs in `pwd` or `worktree`; followed by the **dispatch-shape gate** (existing — `strict` / `grouped` / `single`), which picks the per-cycle batching nested under the chosen strategy. Both gates wait for explicit operator confirmation; both can be pre-committed via flags (`--strategy <name>` / `--isolation <pwd|worktree>` / `--strict` / `--grouped` / `--batch`). Before dispatching each cycle the orchestrator acquires the lease atomically via `planar-agent pull <plan>` (or `planar-agent claim --entity task:<id>` for hand-picked targets) and records the returned `claim_token` in the dispatch entry. With `pwd` isolation, sequential strategies run in the operator checkout. With `worktree` isolation, the orchestrator drives a single sequential lane itself via `workflows/parallel-dispatch.lua --phase cycle_plan`: the seam computes the epic branch, cycle branch, and worktree path, then the model runs the git worktree/branch/merge ops and spawns the coder in that worktree. Under `parallel-fanout`, the orchestrator drives staged worktree fan-out itself via the same seam's `plan`/`waves` phases and spawns N coders concurrently through the host's subagent dispatch surface. There is NO external harness for either path. After the coder reports done, the orchestrator runs **Phase 3.5 — test-coder dispatch** (see below): consults `planar test-spec status <plan> --json` and, when the cycle's dispatched slugs intersect the JSON's `uncovered_task_slugs`, dispatches `pl-test-coder`. The output (coder diff alone or the union of coder + test-coder diffs) is routed through `pl-reviewer` at the cadence chosen by the strategy. The cycle terminates via one of `planar-agent complete` / `fail` / `release` / `block` (atomic — flips both claim status and task status in a single transaction). Enforces the 5-iteration cap per reviewer cycle (the test-coder cycle has its own cap, default 2), and surfaces escalations (open questions, aborts, ship-with-caveats, failure-surfaced).

   **Strategy + isolation gate (first thing Phase 3 does, after reading claim state).** The orchestrator runs the recommendation algorithm against the plan — see [`agents/methodology.md` § Recommendation algorithm](../../agents/methodology.md#recommendation-algorithm) for the rules (mechanical/docs/single-verb → `barrel-bypass`; multi-milestone roadmap with ≤1 parallel-eligible per milestone → `barrel-deferred`; ≥3 tasks with ≥2 parallel-eligible → recommend the model-runnable `parallel-fanout`; single-task → `classic`; otherwise stickiness then `classic`). It then surfaces:

   - the recommended strategy (one of `classic`, `barrel-deferred`, `barrel-bypass`, or `parallel-fanout` — all four are runnable from this skill),
   - the recommended isolation (`pwd` or `worktree`); `pwd` is the continuity default for sequential strategies, `worktree` is selectable for `classic` / `barrel-deferred` / `barrel-bypass`, and `parallel-fanout` always uses worktrees,
   - a one-line rationale (e.g. "2-task plan, neither parallel-eligible" or "4 tasks, 3 parallel-eligible — fan out"),
   - the **dispatch preview** — the task breakdown for the dispatch scope rendered as the ordered `blocks`-subgraph (waves + serialized tasks with exclusion reasons) with the proposed Axis C model tier per task and the routed-model candidate `resolve(role, work_type)` selects within that tier; see [Dispatch preview and model tiers (Axis C)](#dispatch-preview-and-model-tiers-axis-c) below,
   - when the plan is a best architectural fit for `parallel-fanout`, that strategy is **selectable** (it is model-runnable via the `workflows/parallel-dispatch.lua` seam — no external harness); when the operator asks for worktree-backed sequential/deferred execution, confirm the same strategy with `worktree` isolation rather than refusing,
   - the menu of runnable strategies and isolation choices with one-line trade-offs (see [Strategy menu](#strategy-menu) below),
   - the `--strategy custom` escape hatch for axis-by-axis overrides.

   The orchestrator **waits for explicit operator confirmation** before doing any further Phase 3 work (no claim acquisition, no dispatch-shape proposal, no coder dispatch). Auto-defaulting without confirmation is not supported: the recommendation never silently turns into an action. The confirmation covers both the dispatch preview's model-tier column and its routed-model column: the operator may override any task's tier inline (e.g. "task 14 → large") and/or override any task's routed candidate directly (e.g. "task 14 → gpt-5.6-sol") before confirming, and the confirmed tier and candidate are both binding for every subsequent dispatch in the plan.

   The strategy gate is skipped only when `--strategy <name>` (or `--strategy custom --isolation X --branch-model Y ...`) was supplied at invocation. No flag pre-commits Axis C: when the gates are skipped via flags the orchestrator still prints the dispatch preview before the first dispatch, proceeds without prompting while every task sits at the default tier and its routed candidate, but stops for explicit confirmation before dispatching any task it proposes to escalate to `large` and before any cycle containing a `tier: ?` (ambiguous-classification) row. For sequential strategies, `--isolation worktree` is accepted and runnable; if omitted, isolation defaults to `pwd`. `--strategy isolated-sequential` is accepted as an alias for `--strategy classic --isolation worktree`. `--strategy parallel-fanout` is accepted and runnable — it drives staged worktree fan-out via the seam (see [Runnable staged waves](#runnable-staged-waves-m2) below). The dispatch-shape gate then runs nested under the chosen strategy, constrained by it: `barrel-bypass` forces the barrel-bypass shape; `barrel-deferred` forces the barrel-deferred shape; `parallel-fanout` forces the `fan-out` shape; `classic` keeps the full strict / grouped / single menu. The dispatch-shape gate is itself bypassed only when `--strict`, `--grouped`, `--batch`, or a `--barrel-*` standalone flag was supplied (the standalone barrel-* flags are soft-deprecated — see [Aliases and deprecations](#aliases-and-deprecations)).

   **Strategy persistence (live as of migration 00016).** The recommendation algorithm's rule 6 ("if the last dispatch used non-default strategy S, recommend S") rides on the `agent_actions.metadata` JSON column. When the orchestrator confirms a strategy for a cycle, it persists the choice on the dispatch action row by passing `--metadata` to the lease-acquire verb:

   ```sh
   # orchestrator → coder dispatch via plan-pull:
   planar-agent pull <plan-id> --role coder \
     --metadata '{"strategy":"<name>","axes":{...},"dispatch_shape":"<shape>","model_tiers":{"<task-id>":"<tier>"},"rationale":"<text>"}' \
     --json

   # orchestrator → hand-picked task dispatch via direct claim + action start:
   token=$(planar-agent claim --entity task:<id> --role coder --json | jq -r .claim_token)
   planar-agent action start --claim "$token" --kind coder \
     --metadata '{"strategy":"<name>","axes":{...},"model_tiers":{"<task-id>":"<tier>"},"rationale":"<text>"}' --json
   ```

   `--metadata` is validated as well-formed JSON at the CLI parse layer; the engine stores it opaquely.

   At the start of the next cycle's strategy gate the orchestrator reads the most recent dispatch entry for the plan via `planar-watch actions --plan <plan-id> --json` (or `--task <id>` for the hand-picked variant), parses the JSON `metadata` field of the latest non-null row, and applies the recommendation algorithm's rule 6: if the prior strategy is non-default and the plan shape still supports it, recommend the same strategy with a "sticky from prior cycle" rationale. The same read recovers the confirmed `model_tiers` map: prior operator tier overrides are re-proposed in the next dispatch preview with a "sticky from prior cycle" annotation, never silently reverted to the default. The operator still confirms — stickiness only changes the *recommendation*, never the action.

   **Phase 3.5 outcomes:**
   - `expanded` → test-coder diff staged alongside coder's; reviewer sees the union.
   - `no-expansion-needed` → reviewer sees the coder's diff alone.
   - `failure-surfaced` → escalates to the user with the test-coder's report (failing tests classified as `test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`); reviewer NOT dispatched until the user resolves.
   - `abort` → escalates; cycle halts.

4. **Propagation (Phase 4, optional)** — invokes `pl-ext-propagate <plan>` against the registered external system when the user requests `--propagate` or confirms interactively. Never propagates silently.

5. **Archive (Phase 5, optional)** — marks anchor plan `done` and invokes `planar workbench archive <anchor>` when the user requests `--archive` or confirms interactively. Never archives automatically.

5a. **Finalization (Phase 3.7, optional/gated)** — after Phase 3 execution cycles complete with reviewer approval (or barrel gates passed), commits pushed, and a PR open and mergeable, the orchestrator **dispatches the `janitor` agent as a freshly spawned subagent** (same isolation model as coder/reviewer dispatch — Agent/Task tool, subagent type `janitor`, blank context) to run the canonical merge → reconcile → cleanup → `planar plan closeout` flow. See [`agents/janitor.md`](../../agents/janitor.md) for the authoritative six-step sequence.

   **Capability boundary (load-bearing):** The janitor — not the coder, not the orchestrator directly — runs `planar plan closeout`. Coders complete tasks via `planar-agent complete` and **never close plans**. The orchestrator composes the janitor brief and surfaces the result (closed or blocked-with-reasons); it does not call `planar plan closeout` itself. The closeout gate is authoritative: `ready: false` is surfaced to the operator, never forced.

   **Trigger:** explicit operator opt-in (`--finalize` flag at invocation or interactive confirm when the orchestrator offers it after Phase 3). Never triggered automatically by task completion.

   **Brief the orchestrator composes for the janitor:** PR number(s), anchor plan id, worktree/branch details (if any), delivery evidence (reviewer disposition, gate citation), session context (claim token, parent action id for `--parent-action` cross-session hierarchy).

   **Janitor result:**
   - `closed` → plan is `done`, branches/worktrees clean, claims reconciled. Orchestrator records a session note with the closed plan id and merged PR.
   - `blocked-with-reasons` → `planar plan closeout --dry-run` returned `ready: false`; orchestrator surfaces the `blocked_by` list verbatim and stops.

   **Relationship to Archive:** Finalization does merge + DB closeout; Archive does workbench FS archival. They are distinct steps; use `--finalize --archive` to chain them in one invocation.

6. **Documenter (Phase 6, default-on)** — at the end of every cycle (unless `--no-docs` was supplied), runs `planar-doc diff --json`, packages the envelope `{ manifest_path, diff_records, covered_docs, cycle_summary, authoritative_identity, guidance_files }`, dispatches `pl-documenter`, surfaces the returned worklist to the user, and applies each operator-approved row through `planar-doc cover` / `planar-doc nodoc` / a staged doc-body commit, then closes with `planar-doc build`. The documenter only proposes — no `planar-doc` verb fires until the operator approves the row. See [`agents/orchestrator.md` § Phase 6 (Documenter)](../../agents/orchestrator.md#phase-6--documenter-pl-documenter-default-on).

   Derive `authoritative_identity` from repository/build evidence, never from
   guidance prose: `migration_tail` is the lexically greatest five-digit up
   migration with a matching down file; `schema_version` is its prefix checked
   against the tail's `schema_migrations` insert; `binary_set` is exactly
   `planar`, `planar-agent`, `planar-watch`, `planar-doc`, and
   `planar-execute` as installed by `build.zig`; `generated_surface_boundary`
   records `skills/src/` and `agents/` as canonical sources with vendor
   projections generated out of tree, evidenced by `.gitignore` and
   `planar skills render`; `guidance_equivalence` records whether `AGENTS.md`
   and `CLAUDE.md` resolve through a symlink or compare byte-for-byte. Inspect
   those paths, `README.md`, and other repo guidance that explicitly asserts a
   fact. Missing assertions are not drift.

   Every contradiction becomes an operator-gated normal row with
   `signal: guidance-identity-drift`, expected/actual/evidence, and `action:
   defer` for guidance outside `docs/`. Any unresolved row blocks reporting a
   **clean closeout** for Phase 6; zero contradictions invents no rows. There is
   no automatic prose, symlink replacement, manifest write, or plan-closeout
   mutation, and the janitor remains the sole owner of `planar plan closeout`.

## Durable boundary checklist

A verified slice, reviewer decision, wave barrier, operator gate, or failure is
only a candidate boundary. Before yielding, apply this checklist independently
to every task in the dispatch scope whose post-operation status is `todo`,
`doing`, or `blocked`. The surviving target is `task:<id>`; the process, claim,
session, branch, and worktree are attribution to that task, not substitutes for
it. Exclude post-operation `done` and `cancelled` tasks — they are terminal and
must not receive a manufactured resume checkpoint.

For each surviving target:

1. Write `planar task update <task-id> --next-action "<exact-action>"`. The
   value must give a zero-context agent one concrete action, its location
   (command, file/symbol, or named operator gate), and its observable success
   condition, plus any prerequisite absent from the resume packet. Do not use
   vague values such as `continue`, `finish the task`, `implement per
   acceptance criteria`, or `resume work`.
2. Capture a task-linked snapshot with the identical next action:

   ```sh
   planar capture snapshot --task <task-id> \
     --next-action "<same-exact-action>" \
     --note "orchestration_checkpoint: v1
   stage: <verified-slice|reviewer-decision|wave-barrier|operator-gate|failure>
   iteration_scope: <coder-review|test-coder|none>
   iteration: <positive-decimal|0>
   result: <stable-outcome-token>" --json
   ```

   Use the current iteration within `coder-review` or `test-coder`; use
   `iteration_scope: none` and `iteration: 0` where no iteration exists. Take
   `result` from the structured gate, reviewer, terminal, or wave result. Keep
   existing claim, handoff, branch, worktree, commit, and action attribution;
   the snapshot does not replace it.
3. Run `planar resume validate <task-id> --json`. A target is durable only when
   the result says `resumable:true`. Its zero-context continuation command is
   exactly `planar resume <task-id> --json`.

If any target fails validation, return boundary `outcome=partial`; do not
advance or redispatch that target. Name `task:<id>`, preserve every returned
`failures[].check`, `message`, and `remediation`, substitute any known value
into placeholder remediation with shell-safe quoting, and finish with the retry
command `planar resume validate <task-id> --json`. Valid independent targets
remain durable and applied. Heartbeat every surviving held claim at TTL/2 and
immediately after each serial subagent dispatch returns; a checkpoint never
renews a lease.

### `classic` boundary and restart

Apply the checklist after the outcome-producing operation and its atomic
terminal verb, if any, then re-read each task's authoritative status. An
approved task completed through `planar-agent complete` is now `done` and is
excluded. Do not checkpoint it before completion merely to make the cycle look
resumable.

For a nonterminal classic cycle, preserve the outcome that explains why the
task survives. A reviewer `request-changes` decision uses
`stage: reviewer-decision`, `iteration_scope: coder-review`, the current
positive iteration, and `result: request-changes`; its exact next action names
the first concrete remediation, where to apply it, and the gate that proves the
next review is ready. A test-coder `failure-surfaced` or `abort` uses
`stage: failure`, `iteration_scope: test-coder`, its current iteration, and the
literal structured result. An open question, terminal `fail`/`release`/`block`,
or other pause records the corresponding structured result and an exact
operator gate or retry action. Validate before yielding or dispatching the next
iteration.

On a zero-context restart, run `planar resume <task-id> --json` before
reclaiming or redispatching the task. Read the checkpoint's exact stage,
iteration scope, iteration, result, and `next_action` from the packet; do not
infer them from conversation history. Confirm any still-live claim or perform
the packet's explicit recovery action, then execute that exact next action.

### `barrel-deferred` boundary and restart

After every serial coder returns, heartbeat every held claim, verify the slice,
and checkpoint each still-`doing` task before starting another coder. A slice
queued for union review uses `stage: verified-slice`, `iteration_scope: none`,
`iteration: 0`, and `result: slice-verified`; its next action identifies the
milestone or plan union-review boundary, the branch/diff to include, and
reviewer approval as the observable success condition. This checkpoint does
not complete or release the task.

At the deferred reviewer boundary, checkpoint surviving tasks again with
`stage: reviewer-decision`, `iteration_scope: coder-review`, the current union
review iteration, and the literal reviewer result. Run resume validation for
every queued target independently before yielding, retrying review, or adding
another slice. After approval, invoke the existing atomic terminal verb first;
tasks that become `done` are excluded, while any task left `todo`, `doing`, or
`blocked` receives a failure/recovery checkpoint based on its post-terminal
state. For `barrel-bypass`, a successful `complete` is likewise terminal and
excluded; only a nonterminal gate failure or pause is checkpointed before the
operator sees the result.

On restart, run `planar resume <task-id> --json` for every surviving queued
target before reconstructing the union from its existing branch, worktree,
commit, and claim attribution. Preserve the checkpoint's exact stage,
iteration, result, and next action; validate every target again before the
deferred reviewer or next coder dispatch. One invalid target makes the boundary
partial but does not discard independently validated slices.

### `parallel-fanout` wave boundary and restart

At fan-in, perform each lane's merge and atomic terminal operation first, then
run `barrier_check` and classify every lane by its post-operation task status.
A landed lane completed to `done` is terminal and excluded. A `failed_clean`
lane returned to `todo` uses `stage: failure`, `iteration_scope: none`,
`iteration: 0`, and `result: failed-clean`; its exact next action identifies
the lane recompute/claim command and the gate that proves it may fan in. An
abandoned or unmerged lane uses its structured outcome and the exact reconcile,
merge, or operator-conflict action. When the barrier cannot advance because at
least one lane survives, record `stage: wave-barrier` and
`result: wave-partial` for those targets.

Before yielding a partial wave, validate every surviving lane independently.
Do not create a later-wave worktree or redispatch an invalid lane. Return
`outcome=partial` with per-task validation failures while retaining successful
validations and terminal landed lanes exactly as recorded.

On restart, run `planar resume <task-id> --json` for each surviving lane before
`reconcile_plan`, `plan`, or `waves`. Use each packet's exact checkpoint stage,
iteration, result, and next action to reconcile only that lane; then re-run
resume validation independently. Recompute and re-fan only the remaining set
after every target needed by the barrier is either terminal or resumable.

This path wiring does not change the terminal ritual: the orchestrator still
invokes exactly one of `planar-agent complete`, `fail`, `release`, or `block`
per claim, and checkpointing always observes the status produced by that
transaction.

## Quota-aware bounded waves and provider circuit breakers

When provider capacity is unknown, do not launch the full parallel-eligible
set. At the strategy gate, let the operator or vendor surface set a maximum
wave size, record it with the dispatch preview, and start no more than that many
lanes at once. If no maximum was supplied, propose a conservative bounded size
and wait for confirmation. A capacity bound may split one eligible wave into
smaller dispatch waves; it never overrides `blocks`, migration, singleton,
empty-touches, open-question, proposed-decision, or other serialization rules.

Maintain one circuit breaker per provider for the current run. The first
terminal categorized as `usage_limit`, `context_limit`, or `output_limit`
opens only that provider's breaker. From that point:

- start no later not-yet-running lane assigned to the affected provider;
- let every already-running independent lane reach its ordinary terminal
  result, preserving landed work;
- continue eligible lanes on unaffected providers within their own bounded
  waves and dependency barriers;
- run the existing barrier/reconcile path and the durable-boundary checklist
  for every surviving target before yielding.

Opening the breaker does **not** cancel, abort, reconcile, reclaim, or otherwise
mutate a live claim. A clean categorized terminal keeps its atomic result. For
a dead or abandoned claim, report the exact `planar-watch claims --json` and
`planar-agent reconcile --dry-run` inspection commands, then wait for the
operator to choose any abort or reconcile mutation. Never claim cross-lane
rollback; completed lanes and independently written artifacts remain applied.

The orchestration result and recovery packet name the affected provider,
systemic category, `breaker: open`, landed and already-running lanes,
provider-blocked unfinished lanes, unaffected continuing lanes, and each
surviving task's exact `planar resume <task-id> --json` command. Do not add a
provider API call, quota prediction, retry daemon, persisted breaker table, or
background process.

On zero-context resume, re-read the wave's structured claim outcomes, rederive
breaker state per provider, load every surviving target with
`planar resume <task-id> --json`, and recompute only the unfinished eligible
set. Do not dispatch again to an open provider until the operator explicitly
chooses that provider and confirms a new maximum wave size. A new session,
elapsed time, or healthy capacity on another provider is not an implicit reset.
The breaker itself never triggers automatic claim reconciliation.

## User Gates

- Between Phase 1 and Phase 2: user must review artifacts.
- Between Phase 2 preview and `--apply`: user must confirm the diff.
- Phase 3 **strategy + isolation**: user picks (or confirms the recommendation of) one of the four runnable strategies (`classic` / `barrel-deferred` / `barrel-bypass` / `parallel-fanout`) and the isolation mode (`pwd` / `worktree`) where applicable, or supplies `--strategy custom` with per-axis flags. Sequential worktree isolation and `parallel-fanout` are both model-runnable via `workflows/parallel-dispatch.lua` with NO external harness. This gate runs **first** in Phase 3, before claim acquisition or dispatch-shape selection. Skipped only when a valid `--strategy <name>` plus any desired axis overrides was supplied at invocation. The gate includes the dispatch preview (task breakdown, `blocks`-subgraph, per-task tier).
- Phase 3 **model tiers (Axis C)**: the dispatch preview's per-task tier column and routed-model column are both confirmed at the strategy gate, with per-task operator override of either the tier or the candidate before confirming. No flag pre-commits Axis C — when both Phase 3 gates are skipped via flags, default-tier tasks proceed at their routed candidate without prompting but any proposed `large` escalation or `tier: ?` (ambiguous-classification) row still requires explicit confirmation before that dispatch. The dispatch preview itself is unconditional on every path, including hand-picked/direct-claim dispatch.
- Phase 3 **dispatch shape**: user picks one of the dispatch shapes (strict/grouped/single — or the legacy six-shape menu when a standalone barrel-* flag is in play) before any coder runs. Runs nested under the chosen strategy and is constrained by it (`barrel-deferred`/`barrel-bypass` force the corresponding shape; `classic` keeps the full strict / grouped / single menu). Skipped when `--strict`/`--grouped`/`--batch`/`--barrel-grouped`/`--barrel-deferred`/`--barrel-bypass` was supplied.
- Phase 3 claim conflicts: active unexpired claims are not silently bypassed. Stale claims require reconciliation or explicit force-takeover before the work is considered available.
- Phase 3.5 `failure-surfaced` outcome: when a test the test-coder authored fails on first run, user must resolve (fix the test or fix the code) before the reviewer is dispatched. The orchestrator never decides which side is wrong.
- Phase 3.7 (Finalization): user must supply `--finalize` or confirm interactively. The orchestrator never finalizes silently on cycle completion. The `planar plan closeout --dry-run` gate must pass before apply; `ready: false` is surfaced to the operator and no closeout runs.
- Phase 4: user must request propagation.
- Phase 5: user must request archive.
- Phase 6 worklist: user must approve each row before `planar-doc cover` / `nodoc` / a staged doc body / `planar-doc build` fires. `--no-docs` opts out of the phase entirely.

These gates exist to prevent silent side effects on spec/task creation, FS cleanup, plan-status transitions, and the doc-state manifest.

## Claim ritual (`planar-agent`)

The orchestrator's synchronization writes route through the `planar-agent` binary, **not** through a `planar agent` subcommand (the latter does not exist; observability lives on `planar dashboard --agents` / `planar plan next` / `planar-watch` instead). The canonical ritual the orchestrator drives per cycle is:

```text
planar-agent peek <plan>                              # dry-run: what would pull pick?
planar-agent pull <plan> --role coder --json          # atomic: claim + task → doing + start action
planar-agent heartbeat --claim <token> --ttl 600      # at least once per TTL/2 during work
planar-agent complete --claim <token> --summary <s>   # atomic: task → done + claim → completed
# or:
planar-agent fail     --claim <token> --reason <s>    # atomic: task → todo  + claim → aborted
planar-agent release  --claim <token> --reason <s>    # atomic: task → todo  + claim → released
planar-agent block    --claim <token> --blocker <id>  # atomic: task → blocked + edge + claim → released
```

For hand-picked targets, swap `pull <plan>` with `claim --entity task:<id>`; the claim and `todo` → `doing` transition commit atomically. Use `--no-transition` only when the caller deliberately needs the pure claim primitive. For parallel windows, run the heuristic against `entity_links` + task touches to identify mutually-non-conflicting tasks, then issue independent `pull` calls; each returns its own `claim_token`. For operator-side recovery use `planar-agent reconcile [--dry-run]` and `planar-agent abort --claim <token> --reason <text>`.

**Cross-session dispatch hierarchy.** To make a coder's action appear as a child of the orchestrator's own action in `planar-watch tree`, pass `--parent-action <action-id>` to `planar-agent pull`. The orchestrator's action id is the `action_id` field returned by its own `pull` call:

```sh
orch=$(planar-agent pull $PLAN_ID --role orchestrator --json)
orch_action=$(echo "$orch" | jq -r .action_id)
planar-agent pull $PLAN_ID --role coder --parent-action "$orch_action" --json
```

Without `--parent-action`, each pull starts a new root action and `planar-watch tree` shows flat disjoint chains — correct for non-orchestrated work, a gap for orchestrator → coder dispatch. The flag is optional; omitting it preserves today's behavior bit-for-bit.

## Brief composition

The dispatcher's brief is the input the coder runs on. Sloppy briefs are a dispatcher problem to prevent, not a coder problem to recover from. Every coder brief composed by the orchestrator MUST:

- **Cite spec section paths, not paraphrased spec content.** The brief is a pointer to the workbench tech-spec; a paraphrase loses load-bearing detail and silently becomes the coder's source of truth.
- **List task IDs explicitly.** The cycle's scope is the enumerated task list. Task IDs are the contract the reviewer compares the diff against.
- **List claim tokens explicitly.** Claim tokens are the synchronization contract. The reviewer uses them to verify the diff stayed inside leased scope, and interrupted sessions use them for resume/reconcile.
- **Note locked decisions inline.** Patterns like `Q47 = editor markers` or "ADR-0012 forbids new global state here" go in the brief verbatim so the coder does not re-litigate them.
- **Specify the gates the coder must run.** Default Zig gates: `make fmt-check`, `make build`, `make test`, `make test-integration` (run twice, back-to-back), plus `planar skills render --check` against an out-of-tree staging dir when skill/agent surfaces are touched and any remaining relevant validators.
- **Specify the report shape.** Word ceiling and the required sections per [`agents/coder.md` §Work-complete report template](../../agents/coder.md#work-complete-report-template).
- **Pose the problem; do not include the solution.** State the invariant, the constraint, and the acceptance signal. Let the coder design the implementation.
- **Cite test-spec section paths when dispatched tasks have `verifies` edges to test-spec scenarios.** List the cited scenario IDs explicitly so the reviewer can compare the diff against them. Skip the test-spec reference when no scenarios are cited.

See [`agents/methodology.md` §Brief composition discipline](../../agents/methodology.md#brief-composition-discipline) for the full rule.

## Iteration 5 contract

On iteration 5 of a cycle, `request-changes` is invalid; the reviewer must return `approve` (with caveats) or `abort`. The orchestrator treats a `request-changes` returned on iteration 5 as `abort`. Caveats attached to an iteration-5 `approve` are filed as new task rows on the same plan (`planar task add ...`), not buried in commit messages or a "deferred" section. Abort escalates to the user and halts the cycle. See [`agents/methodology.md` §Iteration 5 contract](../../agents/methodology.md#iteration-5-contract).

## Reviewer dispatch profile

Not every cycle benefits from a reviewer pass. The orchestrator picks the reviewer disposition per cycle per [`agents/methodology.md`](../../agents/methodology.md#reviewer-dispatch-profile):

- **Load-bearing (always dispatch reviewer):** architectural / handoff cycles, schema migrations, new CLI surfaces, validate / invariant changes, refactor sweeps with semantic implications.
- **Skip (reviewer adds no signal):** decision-only cycles (recording `planar decision add` rows), pure mechanical sweeps (mass `sed`-style refactors where the diff IS the verification), docs-polish without behavior change.
- **Default reviewer-on** for single-feature additions; flip to skip only if the coder's diff is small and non-architectural.

When skipping the reviewer, the orchestrator records the disposition (and the cycle shape that justifies it) as a session entry so the audit trail captures why no review ran.

## Blind-read contract

When dispatching the reviewer, the orchestrator composes a fresh brief — it MUST NOT paste the coder's or test-coder's full report into the reviewer's context. The reviewer brief contains only: the task IDs, slugs, and claim tokens the coder claimed, the relevant spec/roadmap section paths (not bodies — the reviewer reads the files independently), a directive to run `git diff HEAD` and `git diff --stat HEAD` firsthand, and the coder's quality-gate output (test count, integration confirmation) since the reviewer is not re-running gates. When the test-coder ran successfully, the brief also instructs the reviewer to run `planar test-spec status <plan>` against the post-diff DB and treat any leftover uncovered slug claimed by the brief as a `request-changes` finding. This preserves the reviewer's independent read against the coder/test-coder framing.

## Dispatch preview and model tiers (Axis C)

The strategy gate is not just a strategy question — it is the operator's one look at *what* is about to be dispatched and *at what model tier* before any claim is acquired. The orchestrator renders a **dispatch preview** at the strategy gate, and re-renders it whenever the remaining task set changes shape (a barrel boundary, a wave barrier, a resume after partial failure).

**The preview is unconditional.** No coder dispatch may occur without a dispatch preview rendered earlier in the same session that covers the task(s) being dispatched. This holds on every path: flag-skipped gates (`--strategy` / `--strict` / `--grouped` / `--batch`), hand-picked and direct-claim dispatch (`claim --entity task:<id>`), single-task invocations, and mid-plan resumes. A dispatch whose task never appeared in a rendered preview row is a contract violation, not a shortcut.

**Building the preview.** The claim-aware open set comes from `planar plan next <plan> --json`. The subgraph projection comes from the seam's `waves` phase — `planar-execute run workflows/parallel-dispatch.lua --phase waves --args '{"plan_id":<id>}'` — which is used here as a *presentation projection for every strategy*, not only `parallel-fanout`: under sequential strategies nothing is created from its output; it only orders and labels the picture. Tasks the engine serialized (migration / singleton / empty-touches / open-question / proposed-decision) are shown under `serialized` with their exclusion reasons, never hidden.

**Rendering.** One row per task, grouped by wave, with the `blocks`/`blocked_by` edges, the proposed tier, and the routed-model candidate visible:

```
Phase 3 dispatch preview for plan <p> (<n> open tasks):

  wave 1
    #12  add-parity-gate       blocks: 14         tier: large   model: claude-opus-4-8-thinking  (schema)
    #13  polish-cli-help       —                  tier: medium  model: claude-sonnet-5          (feature)
  wave 2 — unblocks when #12 is done
    #14  wire-handler          blocked_by: 12     tier: medium  model: claude-sonnet-5          (feature)
  serialized — never waved
    #15  backfill-migration    migration guard    tier: large   model: claude-opus-4-8            (engine)
    #16  rework-claim-lease    —                  tier: ?       model: —                        (engine? feature? — touches src/engine/ but follows the extant handler pattern)

  Tiers resolve per agents/models.md §Tier Table. medium is the coder
  default; large is proposed only for schema / engine-judgment /
  architectural work (§Coder tier policy — CLI-surface changes are
  medium). Tiers are per-task: a mixed-tier group is partitioned by
  tier, never inflated to its highest row. A `tier: ?` row means the
  classifier could not decide — the competing signals are shown in the
  tag and that row requires an explicit operator answer before any
  cycle containing it dispatches. The model column is the routed
  candidate within the tier; the one-word work-type tag is shown on
  every row (not only `large` rows) because it also drives the model
  lookup at every tier.

Accept tier assignments and routed candidates? [yes / <task-id> → <tier> / <task-id> → <candidate> ...]
(rows marked `tier: ?` must be answered — `yes` alone does not resolve them)
```

**Tier proposal rule (Axis C).** Per [`agents/models.md` §Coder tier policy](../../agents/models.md#coder-tier-policy): every task defaults to `medium` — Planar's decomposition-first premise means execution is deliberately cheap, and the default is expected to hold for most tasks, including CLI-surface changes. Propose `large` only for schema changes, genuine engine-judgment calls (allocator/error-set/transaction/status-transition design — not routine engine wiring), or large architectural diffs, and annotate the one-word reason in the row. Tiers are per-task and a cycle never inherits its highest task's tier: partition mixed-tier groups by tier or dispatch per-task. When the work-type classification is genuinely ambiguous, render `tier: ?` with the competing signals and require an operator answer — never resolve uncertainty by rounding up to `large`. After two consecutive reviewer bounces on a `medium` cycle that read as capability gaps, a `large` re-proposal may be surfaced at the next preview render (never applied silently); spec ambiguity escalates as an open question instead. Tiers themselves stay abstract (`small`/`medium`/`large`) in the tier column — never name a concrete model identifier as the *tier* assignment; the Tier Table resolves the tier to a model at spawn time. The routed-model column is the deliberate exception: it names the concrete candidate `resolve(role, work_type)` selects, because surfacing that candidate (and letting the operator override it) is the entire point of this column.

**Routed-candidate resolution.** For every task the orchestrator classifies the dominant work type using the same classification that drives Axis C tier escalation ([`agents/models.md` §Coder tier policy](../../agents/models.md#coder-tier-policy) — one of `schema | engine | architectural | cli | feature | mechanical`), then resolves the routed candidate with no new CLI verb: run `planar config show --effective --json` (against the resolved active vendor and the task's confirmed tier) and look up `routing.<vendor>.<tier>.<work_type>`. If that key is present, its `value` is the routed candidate. If it is absent, the routed candidate falls back to the tier default — the `models.<vendor>.<tier>` entry's `value` (`list[0]`). This is exactly the shared resolver's `resolve(role, work_type)` entry point (tech-spec artifact 520, Architecture item 2); the orchestrator does not reimplement the lookup, it reads the same effective-config keys the resolver composes from.

**Confirmation and override.** The tier column and the routed-model column are both part of what the operator confirms at the gate. The operator may override any row's tier (`task 14 → large`, `task 15 → medium`) and/or any row's candidate directly (`task 14 → gpt-5.6-sol`) before confirming; both confirmed maps are **binding**. A tier override with no explicit candidate override re-resolves the candidate at the new tier via `resolve(role, work_type)`; an explicit candidate override stands regardless of tier. The orchestrator spawns each coder subagent at the confirmed tier and candidate and MUST NOT silently deviate from either — if mid-plan evidence suggests a different tier or candidate (a task turned out to touch a migration, or a candidate underperformed), the re-proposal is surfaced at the next preview render, never applied silently.

**Persistence.** The confirmed tier map rides the dispatch entry's `--metadata` JSON as `"model_tiers":{"<task-id>":"<tier>", ...}` (see [Strategy persistence](#phase-behavior) above); the next gate reads it back through the same `planar-watch actions --plan <plan-id> --json` path as strategy stickiness and re-proposes prior overrides with a "sticky from prior cycle" annotation. The confirmed `{tier, candidate, work_type}` triple per task is additionally recorded in the dispatch session entry's `model_choice` map, alongside the existing `model_tiers` map, at the step-8a capture point ([`agents/orchestrator.md` step 8a](../../agents/orchestrator.md)): `model_choice: {"<task-id>":{"tier":"<tier>","candidate":"<model-id>","work_type":"<work-type>"}, ...}`. Per tech-spec D6 this extends the existing note-body convention — no schema migration, no new `session_entries` column — and is the durable record of which concrete candidate a coder actually ran at, and for which work type, recoverable via `planar audit trail --kind plan <plan-id> --grep "^dispatch_shape:"`. The `work_type` field is what `planar models evals` (tech-spec D8) keys its per-(work-type, candidate) scorecard on; the `model_choice` line MUST be well-formed JSON (double-quoted keys/values) — the evals aggregator parses it and skips any note whose line fails to parse or omits `work_type`, treating that history as insufficient-data rather than guessing.

## Strategy menu

Before asking the operator to confirm the strategy gate, the orchestrator surfaces the four strategies it runs and, for sequential strategies, the isolation choice (`pwd` / `worktree`). Strategy answers "what is the overall methodology for this plan?" Isolation answers "where does the coder run?" Dispatch shape (next section) answers "within that strategy, how do I batch *this cycle's* work?"

```
  classic                Sequential cycles, reviewer per cycle,
                         test-coder per cycle. Isolation choice:
                         pwd (continuity default) or worktree.
                         [continuity guarantee — today's behavior bit-for-bit]
                         Recommended for: single-task changes, small plans,
                         high-stakes invariant-touching work.

  parallel-fanout        N coders fanned out, each in its own worktree off
                         a shared epic branch; staged waves respect the
                         `blocks` graph; reviewer at fan-in.
                         [MODEL-RUNNABLE via workflows/parallel-dispatch.lua
                         — no external harness. The seam computes waves /
                         lane paths / merge order and HANDS BACK; the model
                         runs the git worktree/branch/merge ops and spawns
                         the coders.]
                         Recommended for: multi-lane plans (≥3 tasks, ≥2
                         parallel-eligible) where lanes touch disjoint files.

  isolated-sequential    Alias for classic + worktree. Sequential cycle
                         worktrees off an epic branch; reviewer per cycle.
                         [MODEL-RUNNABLE via workflows/parallel-dispatch.lua
                         cycle_plan.]

  barrel-deferred        Coder cycles run back-to-back; reviewer
                         dispatched once at a milestone or plan boundary
                         on the union diff. Isolation choice: pwd or worktree.
                         [throughput + late review safety net] —
                         Recommended for: long sequential plans where
                         per-cycle reviewer overhead exceeds the value.

  barrel-bypass          No reviewer dispatch at all. Quality gates
                         (make fmt-check, build, test, test-integration
                         twice, planar skills render --check against an
                         out-of-tree staging dir, and any remaining
                         relevant validators) ARE the entire signal.
                         Sequential. Isolation choice: pwd or worktree.
                         [maximum throughput; trust the gates] —
                         Recommended for: mechanical sweeps, docs-polish,
                         single-verb additions where the contract is
                         fully gated.

  --strategy custom      Assemble a custom axis combination via
                         --isolation, --branch-model, --concurrency,
                         --reviewer-cadence, --test-coder-cadence.
                         [escape hatch for advanced operators]

  When in doubt: take the recommendation, or use --strategy classic.
```

The named strategies are bundles of the five underlying axes (`isolation`, `branch_model`, `concurrency`, `reviewer_cadence`, `test_coder_cadence`); the axes and the named-bundle table live in [`agents/methodology.md` § Orchestration strategies](../../agents/methodology.md#orchestration-strategies). The orchestrator refuses invalid axis combinations before any dispatch runs: fan-out requires worktree isolation, sequential worktree requires epic-child branches, and in-pwd requires the current branch.

### `classic` — the continuity guarantee

`classic` is the explicit continuity default, not a legacy or deprecated mode. When the operator selects (or accepts the recommendation of) `classic`, the orchestrator's behavior matches today's bit-for-bit:

- **Explicitly skips:** worktree creation, epic branch creation, child-branch creation, fan-in merge, any parallel dispatch.
- **Dispatches:** the coder against the operator's pwd on whatever branch is currently checked out. No `isolation: "worktree"` flag, no `--worktree <path>` on `planar-agent pull`.
- **Reviewer:** runs per cycle, per the existing `agents/methodology.md` reviewer dispatch profile.
- **Test-coder:** runs per cycle when uncovered slugs intersect the cycle's slugs, per Phase 3.5.

The promise: introducing the strategy menu does not require existing operators to learn a new flow to keep working as they do. Pick `classic`, get today's behavior. See [`agents/methodology.md` § Continuity guarantee: `classic`](../../agents/methodology.md#continuity-guarantee-classic) for the framing.

### Worktree Isolation (`cycle_plan` and `parallel-fanout`)

Worktree isolation is **fully model-runnable**. The deterministic branch/path
bookkeeping lives in the spawn-free `workflows/parallel-dispatch.lua` seam, and
this skill drives it:

- `cycle_plan` computes one sequential lane for `classic --isolation worktree`,
  `barrel-deferred --isolation worktree`, `barrel-bypass --isolation worktree`,
  and the `isolated-sequential` alias. It does not consult parallel eligibility
  and does not require `task_touches`.
- `plan` / `waves` compute staged multi-lane fan-out for `parallel-fanout`.

The seam COMPUTES (epic branch, lane branch, worktree path, staged waves,
contract-lanes-first fan-in merge order, retention/teardown lists,
boundary-conflict escalation, and the reconcile plan) and HANDS BACK; the model
runs the git worktree/branch/merge ops and spawns coders through the host's
subagent dispatch surface. The seam never spawns and never touches `git worktree`/`git merge` —
re-adding a model-spawning primitive is the exact scope creep that got the old
`planar-execute` extracted. There is **no external harness** in this path
(product-spec §Non-Goals "Not reviving centurion or any external harness"); the
runner IS the model orchestrator plus existing Planar primitives.

#### Runnable sequential worktree cycle

Use this path whenever the confirmed strategy is `classic`, `barrel-deferred`,
or `barrel-bypass` with `--isolation worktree`.

1. **Select the task normally.** Use `planar plan next <plan> --json` /
   `planar-agent peek <plan>` to identify the next task. Sequential worktree
   isolation does not require parallel eligibility and does not require
   `task_touches`.
2. **Compute the lane.** Run:
   `planar-execute run workflows/parallel-dispatch.lua --phase cycle_plan --args '{"plan_id":<id>,"task_id":<task>}'`.
   The seam emits `epic_branch`, `lane.branch`, and `lane.worktree`.
3. **Epic-cut precondition.** Before cutting the epic branch, verify the main
   checkout is clean. If dirty, fail fast with the dirty paths.
4. **Create or reuse the epic worktree.** Create `epic/p<plan>-<slug>` from the
   clean base once per plan; reuse it for later sequential cycles.
5. **Create the cycle worktree.** `git worktree add <lane.worktree> -b
   <lane.branch> <epic_branch>`.
6. **Claim with the worktree path.** Use `planar-agent pull <plan> --role coder
   --worktree <lane.worktree> --json` (or `claim --entity task:<id> --worktree
   <lane.worktree>` for hand-picked targets). The persisted worktree path is the
   resume/recovery key.
7. **Spawn the coder in the cycle worktree.** The coder inherits cwd and must not
   create, move, or remove worktrees.
8. **Fan-in after terminal success.** Merge the cycle branch into the epic
   worktree. For `classic`, run reviewer/test-coder per cycle. For
   `barrel-deferred`, keep merging successful cycles into epic and dispatch the
   reviewer/test-coder once at the milestone/plan boundary on the union diff. For
   `barrel-bypass`, gates are the review signal.
9. **Cleanup.** Remove succeeded cycle worktrees eagerly; retain failed lanes for
   inspection; retain the epic branch/worktree until finalization/PR merge.

In-flight worktree execution is watched through the existing `planar-watch ps --plan <id>`
surface (lane claims with each claim's `worktree_path`, live vs. stale) — there
is no dedicated wave/barrier view (a recorded non-goal).

#### Runnable single-wave fan-out (M1)

1. **Compute the wave.** Run the seam's `plan` phase:
   `planar-execute run workflows/parallel-dispatch.lua --phase plan --args '{"plan_id":<id>}'`.
   It reads `planar plan recommend-strategy <id> --json` (the SINGLE eligibility
   gate — the six parallel-eligibility rules are never re-derived here) and emits
   byte-stable JSON: `{ fan_out, epic_branch, base, lanes:[{task_id, slug,
   branch, worktree}], serialized:[…] }`. The currently-eligible tasks ARE the
   current wave (pairwise-disjoint by construction). Fewer than two eligible
   tasks ⇒ `fan_out:false` with an empty lane list — fall back to the in-pwd
   path (do NOT cut an epic branch for a single lane).
2. **Present at the strategy gate.** Show the operator the epic branch, each
   lane's task + branch + worktree path, and the serialized tasks with their
   exclusion reasons. Wait for explicit confirmation before creating anything.
3. **Epic-cut precondition.** Before cutting the epic branch, verify the working
   tree is clean. If it is dirty, FAIL FAST with an error naming the dirty paths
   rather than cutting `epic/p<plan>-<slug>` over uncommitted work (a dirty base
   silently folds local changes into every lane). Then cut the epic branch once
   from the base ref.
4. **Create per-lane worktrees.** For each lane, `git worktree add <worktree>
   -b <branch> <epic-branch>` on the seam-computed `cycle/p<plan>/<task-slug>`
   branch.
5. **Fan out N coders concurrently.** In ONE turn, spawn N `coder` subagents through
   the host's subagent dispatch surface, each with `isolation: worktree` pointing at its lane
   worktree, each acquiring its own claim with `planar-agent pull <plan> --role
   coder --worktree <path>`. Each coder heartbeats and RETURNS its diff on its
   lane branch — it does NOT fire a terminal verb. (Axis A isolation is
   non-negotiable: each coder is still a separately spawned subagent.)
6. **Manual fan-in.** After all lanes report done, run the seam's `fan_in` phase
   (`--phase fan_in --args '{"plan_id":<id>,"lanes":[…]}'`) to get the stable
   (task-id-ordered) merge order, the eager teardown list, and the retained list.
   Each lane MAY carry `"outcome":"succeeded"|"failed"` (default `succeeded`): a
   SUCCEEDED lane is merged and appears in `teardown_worktrees` (torn down
   eagerly); a FAILED lane is NOT merged and appears in `retained_worktrees` —
   its worktree is KEPT for inspection (tech-spec Retention rule). Merge each
   succeeded lane branch into the epic branch in `merge_order` with `git merge
   --no-ff <lane-branch>`, then `git worktree remove <worktree>` for each path in
   `teardown_worktrees` only (never a retained failed lane). The orchestrator
   owns the per-lane terminal verb: fire exactly one `planar-agent complete` per
   landed lane at fan-in (a lane counts as landed only once its branch is fanned
   in AND its terminal verb fired).

Naming is locked (tech-spec §Decisions): epic `epic/p<plan>-<plan-slug>`, lane
`cycle/p<plan>/<task-slug>`. Do not invent alternate names — the seam computes
them and the existing worktree-cleanup backstop keys off the convention.

#### Runnable staged waves (M2)

For a plan whose lanes have `blocks` dependencies, present the FULL ordered wave
plan at the strategy gate and enforce the wave barrier between waves. Waves are
NOT partitioned up front to gate eligibility — the engine's `recommend-strategy`
stays the single eligibility gate. The seam walks the plan's `blocks` graph ONLY
to ORDER and LABEL the waves for the operator; waves EMERGE by iterative
recompute across barriers (each barrier re-runs `recommend-strategy`, and a
downstream lane becomes eligible only once its `blocks`-blocker is `done`).

1. **Present the ordered wave plan at the gate.** Run the seam's `waves` phase:
   `planar-execute run workflows/parallel-dispatch.lua --phase waves --args '{"plan_id":<id>}'`.
   It emits byte-stable JSON `{ epic_branch, base, wave_count, waves:[{ wave,
   lane_count, lanes:[{task_id, slug, branch, worktree, blocked_by:[…]}] }],
   serialized:[…] }`. Show the operator the FULL ordered picture — each wave's
   lanes with their per-lane branch + worktree path, the `blocked_by` blocker
   task ids that gate each later wave (so they see "wave 2 unblocks once proto
   lands"), and the serialized tasks with their exclusion reasons. Wait for
   explicit confirmation before creating any worktree. A strict blocks-chain
   collapses to sequential one-lane waves; a fully-parallel set is one wave.
   A task the engine serialized for a NON-blocks reason (migration / singleton /
   empty-touches / open-question / proposed-decision) is carried in `serialized`
   and NEVER placed in a wave, regardless of its blocks-depth.
2. **Drive ONE barrier at a time.** The `waves` output is the projection for the
   gate, not a schedule the seam executes. For the CURRENT wave, run the `plan`
   phase (which returns exactly the currently-eligible lanes), create its
   worktrees, and fan out its coders as in the single-wave path above.
3. **Enforce the wave barrier before the next wave.** After fanning the current
   wave in, run the seam's `barrier_check` phase
   (`--phase barrier_check --args '{"plan_id":<id>,"lanes":[{"task_id":…,"landed":…,"fanned_in":…}]}'`).
   It emits `{ proceed, blocking:[{task_id, reason}] }`. Do NOT create any
   wave-N+1 worktree until `proceed:true`. The barrier is on FAN-IN completion,
   not coder completion: a lane whose coder reported done (`landed:true`) but
   whose branch has not been merged into the epic (`fanned_in:false`) does NOT
   satisfy the barrier — `barrier_check` names it as blocking with reason
   `not_fanned_in`. Once `proceed:true`, re-run the `plan` phase (the just-landed
   lanes have dropped out of `recommend-strategy` and their dependents are now
   eligible) and repeat for the next wave.

#### Fan-in conflict, partial-wave failure/resume, and teardown (M3–M4)

These paths are now fully runnable from this skill via the seam:

- **Capacity recovery (plan 858 M3).** After supplied lane outcomes include a
  systemic `usage_limit`, `context_limit`, or `output_limit`, call
  `--phase capacity_reconcile` with each lane's `task_id`, `provider`,
  `outcome` (`landed|failed_clean|abandoned|pending|running`), and failure
  `category` where applicable. Preserve the returned `landed`, `running`, and
  `unaffected` sets and operate only on `unfinished`. Report each open provider
  breaker and the `recovery` packet verbatim: run the per-task `planar resume
  <task-id> --json` reads, use the claim inspection and reconcile dry-run
  commands only as explicit operator recovery, and require the operator's
  `explicit_dispatch` plus a newly confirmed maximum wave size before treating
  a provider reset as chosen. The phase does not execute any recovery command.
- **Boundary conflict (M3).** When a `git merge --no-ff <lane>` conflicts, run
  `git merge --abort`, then call `--phase conflict_escalation --args
  '{"plan_id":<id>,"ours":<epic-side branch>,"theirs":<lane branch>,"paths":[…]}'`
  to format the escalation payload (sorted conflicting paths, both branches,
  `auto_resolved:false`, `action:"abort"`). Surface it to the operator and pause.
  The seam NEVER auto-resolves — the confined `git` host group has no merge verb.
- **Partial-wave failure + resume (M3).** On a mid-wave failure, the other lanes
  run to completion and the barrier does not advance (enforced by
  `barrier_check`). On resume, call `--phase reconcile_plan --args
  '{"plan_id":<id>,"lanes":[{"task_id":…,"outcome":"landed"|"failed_clean"|"abandoned"}]}'`.
  A `failed_clean` lane (coder's atomic `planar-agent fail`) needs NO reconcile —
  it is available again on the next recompute. An `abandoned` lane (dead coder,
  stranded claim) is surfaced with the exact reclaim argv `planar-agent reconcile
  --stale-after 0` (immediate, not waiting out the lease TTL). Then re-run the
  `plan`/`waves` phase — landed lanes have dropped out of `recommend-strategy`,
  so only the remainder re-fans-out (resume is "recompute + re-fan the
  remainder," never "restart from wave 1").
- **Full teardown on plan completion (M4).** When the plan is complete, call
  `--phase teardown --args '{"plan_id":<id>,"epic_branch":<epic>,"lanes":[{"task_id":…,"branch":…,"worktree":…}]}'`
  to get `remove_worktrees` (every lane worktree still on disk) and
  `delete_branches` (every lane branch) for `git worktree remove` / `git branch
  -D`. The epic branch is surfaced as `retained_branch` and is RETAINED until its
  PR merges — never in the removal lists.

## Dispatch shape options

Once the strategy is chosen, the orchestrator runs the dispatch-shape gate **nested under the strategy**. The shape describes per-cycle batching, not overall methodology. Under `classic` the operator picks freely from `strict` / `grouped` / `single`. Under `barrel-deferred` and `barrel-bypass` the shape is forced to the matching barrel shape. Under `parallel-fanout` the shape is forced to `fan-out` (N parallel coders per wave, one reviewer at fan-in).

```
  strict           One coder cycle per task; full reviewer per task.
                   [safe; slow] — Recommended for logic changes,
                   multi-file edits, spec/schema changes.

  grouped          Orchestrator picks groupings (typically by milestone
                   or shared file scope); reviewer per group.
                   [balanced] — Useful when tasks are tightly coupled.

  single           All tasks in one coder cycle; one reviewer pass.
                   [tiny features only] — Decomposition is theatre.

  fan-out          (forced under --strategy parallel-fanout) N parallel
                   coders + N worktrees per wave, staged by the `blocks`
                   graph, single reviewer at fan-in. Model-runnable via
                   workflows/parallel-dispatch.lua — no external harness.

  barrel-deferred  (forced under --strategy barrel-deferred) Coder
                   cycles run back-to-back; reviewer fires at the
                   configured boundary (milestone default;
                   --barrel-deferred-at plan for once-per-plan).

  barrel-bypass    (forced under --strategy barrel-bypass) No reviewer
                   dispatch at all; gates are the entire signal.

  When in doubt: use --strict.
```

The three `barrel-*` shapes are documented in detail in [`agents/methodology.md` § Barrel modes](../../agents/methodology.md#barrel-modes). Phase 3.5 (test-coder dispatch) fires across all barrel modes when uncovered slugs intersect the cycle's slugs. `barrel-bypass` bypasses the reviewer, not the coverage gate. See [`agents/methodology.md` § Dispatch mode selection](../../agents/methodology.md#dispatch-mode-selection) for the full inline-vs-strict rule.

## Aliases and deprecations

The pre-strategy-model dispatch-shape gate exposed six standalone flags (`--strict`, `--grouped`, `--batch`, `--barrel-grouped`, `--barrel-deferred`, `--barrel-bypass`). Under the strategy model, the three `--barrel-*` standalone flags are **soft-deprecated**: they continue to work as documented, but they conflate the strategy choice with the dispatch-shape choice. The preferred form is `--strategy <name>`.

| Deprecated standalone flag | Preferred form | Strategy implied |
|----------------------------|----------------|------------------|
| `--barrel-grouped`         | `--grouped` (under any non-barrel strategy) | n/a — was always an alias for grouped |
| `--barrel-deferred [--barrel-deferred-at ...]` | `--strategy barrel-deferred` | `barrel-deferred` |
| `--barrel-bypass`          | `--strategy barrel-bypass` | `barrel-bypass` |

When the orchestrator sees a standalone `--barrel-*` flag at invocation, it accepts it (preserving operator muscle memory and existing scripts) but emits a one-line deprecation note before Phase 3 proceeds:

```
note: --barrel-deferred is now an alias for --strategy barrel-deferred;
      the standalone flag will be removed in a future cycle. See
      agents/methodology.md § Orchestration strategies.
```

The deprecation note is informational, not blocking. Both gates are still skipped (the standalone barrel-* flag pre-commits both the strategy and the dispatch shape, same as the original semantics). The note exists to surface the migration path to operators using the old form.

`--strict`, `--grouped`, and `--batch` are **not** deprecated — they remain first-class dispatch-shape skips and compose with `classic` (the only strategy that admits the full strict / grouped / single menu); they are refused under the forced-shape strategies (`barrel-deferred`, `barrel-bypass`, and `parallel-fanout`, which forces `fan-out`). Removal of the standalone `--barrel-*` flags is a future cycle's decision, not this cycle's.

The deprecation only touches Axis B (reviewer disposition / dispatch shape). It does not weaken Axis A (the isolation invariant from § "Isolation invariant" above — every coder runs in a spawned subagent regardless of which strategy or shape the operator picks). See `agents/methodology.md` § "Dispatch mode selection" for the full two-axis dispatch model (Axis A: isolation, non-negotiable; Axis B: reviewer disposition, tunable).

## Status reporting

The orchestrator emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings are:

| Phase | Status string |
|-------|---------------|
| Dispatching a coder cycle | `"dispatching coder: task <id>"` or `"dispatching coder: cycle <n>"` |
| Waiting for coder to return its terminal verb | `"awaiting:coder"` |
| Dispatching a test-coder cycle | `"dispatching test-coder"` |
| Waiting for test-coder to return | `"awaiting:test-coder"` |
| Composing the reviewer brief | `"composing reviewer brief"` |
| Waiting for reviewer to return its decision | `"awaiting:reviewer"` |
| Processing the reviewer's decision (approve / request-changes / abort) | `"handling reviewer result"` |
| Starting the next coder iteration | `"dispatching coder: cycle <n+1>"` |
| Dispatching the janitor (Phase 3.7) | `"dispatching janitor: plan <id>"` |
| Waiting for janitor to return its result | `"awaiting:janitor"` |
| Surfacing janitor result (closed or blocked) | `"handling janitor result: plan <id>"` |

Wait states use the `awaiting:` prefix so the read surface (`planar-watch ps`) can distinguish "blocked on something external" from "actively working." Plain text (no prefix) means the orchestrator is actively coordinating. The cap on `--status` payload is 256 bytes.

See [`agents/orchestrator.md` § Status reporting](../../agents/orchestrator.md#status-reporting) and [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the full contract.

Every phase-boundary and final operator response keeps the orchestrator's
canonical decision records: phase selection, strategy/isolation and dispatch
shape gates, claim routing, subagent decisions, iteration state, and any
operator approval still required. The shared fields below summarize that
stronger orchestration state; they do not replace or flatten it.

## Context

Report the resolved scope, goal/plan/task targets, active phase, selected mode,
strategy, isolation, dispatch shape, and claim tokens relevant to the result.

## Intent

State in one sentence which lifecycle transition or execution scope the
orchestrator interpreted from the operator's request.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts across phase
targets. For multi-cycle or multi-target work, retain per-task claim,
test-coder, reviewer, merge, propagation, archive, and documentation outcomes;
do not claim one transaction across independent worktrees or remote calls.

## Result

Always report `outcome=ok|partial|error` and the verified lifecycle post-state:
plan/task identifiers and statuses, surviving claims, commits/branches, remote
URLs, or manifest root as applicable. Preserve every canonical gate choice,
subagent verdict, iteration-cap decision, and pending operator approval.

## Warnings

Name partial failures, stale or mismatched claims, degraded validation,
unmerged worktrees, deferred documentation, unresolved questions, and
consequential assumptions. An expected gate pause or clean no-op is not itself
a warning.

## Next actions

Give zero to three executable recommendations ordered by usefulness. When the
workflow is paused at an operator gate, put the exact approval choice or CLI
continuation first; otherwise point to the next phase, inspection, or safe
terminal routing action.

## Recovery

For partial or failed orchestration, name every affected target and give its
exact idempotent inspect, retry, resume, or cleanup command. Completed
independent targets remain applied unless the underlying verb is atomic. The
orchestrator invokes exactly one terminal `planar-agent` verb per claim and
never invents rollback for worktree merges or remote propagation.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
