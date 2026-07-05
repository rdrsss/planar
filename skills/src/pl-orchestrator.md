---
slug: pl-orchestrator
description: "Run the orchestrator over a goal, anchor plan, or task list — manage the full feature lifecycle (planning, ingestion, execution, finalization, propagation, archive) with reviewer iteration cap and user gates at each phase boundary."
source: agents/orchestrator.md
model_tier: large
vendor:
  claude:
    argument_hint: "<goal|plan-id|task-id> [<task-id>...] [--finalize] [--propagate] [--archive] [--strategy <name>] [--no-docs] [--strict | --grouped | --batch <ids>]"
    invocation_examples: |
      /orchestrator <goal>                          # start from scratch: plan → wait → ingest → wait → execute
      /orchestrator <anchor-plan-id>                # resume from current anchor plan status
      /orchestrator <task-id> [<task-id>...]        # execute specific tasks (Phase 3 only)
      /orchestrator <goal> --propagate              # plan → ingest → execute → propagate
      /orchestrator <anchor-plan-id> --finalize     # execute → dispatch janitor → merge + reconcile + closeout
      /orchestrator <anchor-plan-id> --finalize --archive  # execute → finalize → archive FS tree
      /orchestrator <anchor-plan-id> --archive      # execute → mark done → archive FS tree
      /orchestrator <plan-id> --strategy classic              # explicit continuity — coder in pwd, current branch, sequential
      /orchestrator <plan-id> --strategy barrel-deferred      # back-to-back coder cycles in pwd; reviewer at boundary
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

**"Dispatch to a coder" means spawning a subagent, not invoking `/pl-coder` inline.** A slash command runs in the caller's own context and model — that collapses the orchestrator and coder into a single agent, which is the defect this rule prevents. Spawn a fresh subagent via the harness Agent/Task tool (subagent type `coder`).

This is Axis A of the two-axis dispatch model and it is non-negotiable. It is independent of:
- **File count or edit triviality.** Even a one-line doc fix must go through a spawned coder.
- **Model tier.** Even when the orchestrator is already running at `large` (opus), the coder must be a separate spawned subagent. "I'm already the best model, spawning adds nothing" is a rationalization that violates isolation.

**Axis B (reviewer disposition)** is the tunable axis: whether a reviewer pass runs after the coder. The six dispatch shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`) and the reviewer-dispatch profile are Axis B — they govern reviewer behavior, not coder isolation.

## Phase Behavior

The orchestrator selects phases based on the anchor plan's current `status`:

1. **Planning (Phase 1)** — anchor plan in `draft` with no workbench artifacts: invokes `pl-spec-draft "<goal>"`, surfaces the drafted artifacts to the user, and **waits for explicit review** before proceeding. Does not auto-advance.

2. **Ingestion (Phase 2)** — anchor plan in `draft` with workbench artifacts present: invokes `pl-spec-ingest <plan>` in preview mode (no `--apply`), presents the diff to the user, and **waits for explicit confirmation** before running `--apply`. Never auto-applies.

3. **Execution (Phase 3)** — anchor plan `active` or `paused`: first reads claim-aware state with `planar plan next <plan>` (operator-side) or `planar-agent peek <plan>` (agent-side dry-run for explicit task IDs), excludes active unexpired claims, and surfaces stale claims before dispatch. Phase 3 then runs **two gates in order** before any coder runs: the **strategy gate** (new — see below), which picks the overall in-pwd methodology for the plan (`classic` / `barrel-deferred` / `barrel-bypass`; the worktree strategies `isolated-sequential` / `parallel-fanout` are owned by an external workflow harness), followed by the **dispatch-shape gate** (existing — `strict` / `grouped` / `single`), which picks the per-cycle batching nested under the chosen strategy. Both gates wait for explicit operator confirmation; both can be pre-committed via flags (`--strategy <name>` / `--strict` / `--grouped` / `--batch`). Before dispatching each cycle the orchestrator acquires the lease atomically via `planar-agent pull <plan>` (or `planar-agent claim --entity task:<id>` for hand-picked targets) and records the returned `claim_token` in the dispatch entry. The model orchestrator runs cycles **sequentially in pwd**. Parallel fan-out — consulting the `entity_links` graph + task touches to find the parallel-eligible subset and dispatching N coders into N worktrees — is owned by an external workflow harness; when a plan is a fit, the strategy gate surfaces that. This skill runs classic and barrel strategies only. After the coder reports done, the orchestrator runs **Phase 3.5 — test-coder dispatch** (see below): consults `planar test-spec status <plan> --json` and, when the cycle's dispatched slugs intersect the JSON's `uncovered_task_slugs`, dispatches `pl-test-coder`. The output (coder diff alone or the union of coder + test-coder diffs) is routed through `pl-reviewer`. The cycle terminates via one of `planar-agent complete` / `fail` / `release` / `block` (atomic — flips both claim status and task status in a single transaction). Enforces the 5-iteration cap per coder/reviewer cycle (the test-coder cycle has its own cap, default 2), and surfaces escalations (open questions, aborts, ship-with-caveats, failure-surfaced).

   **Strategy gate (first thing Phase 3 does, after reading claim state).** The orchestrator runs the recommendation algorithm against the plan — see [`agents/methodology.md` § Recommendation algorithm](../../agents/methodology.md#recommendation-algorithm) for the rules (mechanical/docs/single-verb → `barrel-bypass`; multi-milestone roadmap with ≤1 parallel-eligible per milestone → `barrel-deferred`; ≥3 tasks with ≥2 parallel-eligible or worktree isolation would help → surface as harness-fit context and recommend best in-pwd strategy; single-task → `classic`; otherwise stickiness then `classic`). It then surfaces:

   - the recommended **runnable** strategy (one of the three in-pwd strategies: `classic`, `barrel-deferred`, `barrel-bypass`),
   - a one-line rationale (e.g. "2-task plan, neither parallel-eligible"),
   - when the plan is a best architectural fit for a harness-owned strategy (`isolated-sequential` / `parallel-fanout`), a note such as: *"Best architectural fit is `parallel-fanout` (worktree fan-out), but that requires the external worktree harness, which is not runnable from this skill — so the actionable choice here is the best in-pwd strategy below."*
   - the menu of the three in-pwd strategies with one-line trade-offs (see [Strategy menu](#strategy-menu) below), plus an informational pointer to the two harness-owned strategies,
   - the `--strategy custom` escape hatch for axis-by-axis overrides.

   The orchestrator **waits for explicit operator confirmation** before doing any further Phase 3 work (no claim acquisition, no dispatch-shape proposal, no coder dispatch). Auto-defaulting without confirmation is not supported: the recommendation never silently turns into an action.

   The strategy gate is skipped only when `--strategy <name>` (or `--strategy custom --isolation X --branch-model Y ...`) was supplied at invocation. If `--strategy parallel-fanout` or `--strategy isolated-sequential` is supplied, the orchestrator **refuses** with: *"Strategy `<name>` requires the external worktree harness, which is not runnable from this skill. Select `classic`, `barrel-deferred`, or `barrel-bypass` instead."* The dispatch-shape gate then runs nested under the chosen in-pwd strategy, constrained by it: `barrel-bypass` forces the barrel-bypass shape; `barrel-deferred` forces the barrel-deferred shape; `classic` keeps the full strict / grouped / single menu. The dispatch-shape gate is itself bypassed only when `--strict`, `--grouped`, `--batch`, or a `--barrel-*` standalone flag was supplied (the standalone barrel-* flags are soft-deprecated — see [Aliases and deprecations](#aliases-and-deprecations)).

   **Strategy persistence (live as of migration 00016).** The recommendation algorithm's rule 6 ("if the last dispatch used non-default strategy S, recommend S") rides on the `agent_actions.metadata` JSON column. When the orchestrator confirms a strategy for a cycle, it persists the choice on the dispatch action row by passing `--metadata` to the lease-acquire verb:

   ```sh
   # orchestrator → coder dispatch via plan-pull:
   planar-agent pull <plan-id> --role coder \
     --metadata '{"strategy":"<name>","axes":{...},"dispatch_shape":"<shape>","rationale":"<text>"}' \
     --json

   # orchestrator → hand-picked task dispatch via direct claim + action start:
   token=$(planar-agent claim --entity task:<id> --role coder --json | jq -r .claim_token)
   planar-agent action start --claim "$token" --kind coder \
     --metadata '{"strategy":"<name>","axes":{...},"rationale":"<text>"}' --json
   ```

   `--metadata` is validated as well-formed JSON at the CLI parse layer; the engine stores it opaquely.

   At the start of the next cycle's strategy gate the orchestrator reads the most recent dispatch entry for the plan via `planar-watch actions --plan <plan-id> --json` (or `--task <id>` for the hand-picked variant), parses the JSON `metadata` field of the latest non-null row, and applies the recommendation algorithm's rule 6: if the prior strategy is non-default and the plan shape still supports it, recommend the same strategy with a "sticky from prior cycle" rationale. The operator still confirms — stickiness only changes the *recommendation*, never the action.

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

6. **Documenter (Phase 6, default-on)** — at the end of every cycle (unless `--no-docs` was supplied), runs `planar-doc diff --json`, packages the envelope `{ manifest_path, diff_records, covered_docs, cycle_summary }`, dispatches `pl-documenter`, surfaces the returned worklist to the user, and applies each operator-approved row through `planar-doc cover` / `planar-doc nodoc` / a staged doc-body commit, then closes with `planar-doc build`. The documenter only proposes — no `planar-doc` verb fires until the operator approves the row. See [`agents/orchestrator.md` § Phase 6 (Documenter)](../../agents/orchestrator.md#phase-6--documenter-pl-documenter-default-on).

## User Gates

- Between Phase 1 and Phase 2: user must review artifacts.
- Between Phase 2 preview and `--apply`: user must confirm the diff.
- Phase 3 **strategy**: user picks (or confirms the recommendation of) one of the three in-pwd strategies (`classic` / `barrel-deferred` / `barrel-bypass`) that this skill actually runs, or supplies `--strategy custom` with per-axis flags. Harness-owned strategies (`isolated-sequential` / `parallel-fanout`) are surfaced as informational context only — they are not selectable here; if supplied via `--strategy`, the skill refuses with a clear "not runnable in this skill — requires the external worktree harness" message. This gate runs **first** in Phase 3, before claim acquisition or dispatch-shape selection. Skipped only when a valid `--strategy <name>` was supplied at invocation.
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

For hand-picked targets, swap `pull <plan>` with `claim --entity task:<id>` (does NOT auto-transition task status — caller decides). For parallel windows, run the heuristic against `entity_links` + task touches to identify mutually-non-conflicting tasks, then issue independent `pull` calls; each returns its own `claim_token`. For operator-side recovery use `planar-agent reconcile [--dry-run]` and `planar-agent abort --claim <token> --reason <text>`.

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

## Strategy menu

Before asking the operator to confirm the strategy gate, the orchestrator surfaces the three in-pwd strategies it runs — plus a pointer to the two worktree strategies owned by an external workflow harness — with a one-line trade-off each, plus the `--strategy custom` escape hatch. Strategy answers "what is the overall methodology for this plan?" — dispatch shape (next section) answers "within that strategy, how do I batch *this cycle's* work?"

```
  classic                Coder runs in operator's pwd on the current
                         branch. Sequential cycles, reviewer per cycle,
                         test-coder per cycle. No worktrees, no epic
                         branch, no parallelism.
                         [continuity guarantee — today's behavior bit-for-bit]
                         Recommended for: single-task changes, small plans,
                         high-stakes invariant-touching work.

  isolated-sequential    Worktree on an epic-child branch; sequential.
  parallel-fanout        N coders fanned out, each in its own worktree;
                         reviewer at fan-in.
                         [both OWNED BY an external workflow harness, NOT
                         run by this model orchestrator — use an external
                         harness for worktree / parallel execution]

  barrel-deferred        Coder cycles run back-to-back in pwd; reviewer
                         dispatched once at a milestone or plan boundary
                         on the union diff. No isolation.
                         [throughput + late review safety net] —
                         Recommended for: long sequential plans where
                         per-cycle reviewer overhead exceeds the value.

  barrel-bypass          No reviewer dispatch at all. Quality gates
                         (make fmt-check, build, test, test-integration
                         twice, planar skills render --check against an
                         out-of-tree staging dir, and any remaining
                         relevant validators) ARE the entire signal.
                         Sequential, in-pwd.
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

The named strategies are bundles of the five underlying axes (`isolation`, `branch_model`, `concurrency`, `reviewer_cadence`, `test_coder_cadence`); the axes and the named-bundle table live in [`agents/methodology.md` § Orchestration strategies](../../agents/methodology.md#orchestration-strategies). The orchestrator refuses the invalid axis combinations listed there (e.g. `concurrency=fan-out` with `isolation=in-pwd`) with a diagnostic before any dispatch runs.

### `classic` — the continuity guarantee

`classic` is the explicit continuity default, not a legacy or deprecated mode. When the operator selects (or accepts the recommendation of) `classic`, the orchestrator's behavior matches today's bit-for-bit:

- **Explicitly skips:** worktree creation, epic branch creation, child-branch creation, fan-in merge, any parallel dispatch.
- **Dispatches:** the coder against the operator's pwd on whatever branch is currently checked out. No `isolation: "worktree"` flag, no `--worktree <path>` on `planar-agent pull`.
- **Reviewer:** runs per cycle, per the existing `agents/methodology.md` reviewer dispatch profile.
- **Test-coder:** runs per cycle when uncovered slugs intersect the cycle's slugs, per Phase 3.5.

The promise: introducing the strategy menu does not require existing operators to learn a new flow to keep working as they do. Pick `classic`, get today's behavior. See [`agents/methodology.md` § Continuity guarantee: `classic`](../../agents/methodology.md#continuity-guarantee-classic) for the framing.

### Worktree strategies (`isolated-sequential`, `parallel-fanout`)

`isolated-sequential` still requires deterministic worktree creation, epic/cycle
branch management, and fan-in merging owned by an external workflow harness — it
is not runnable from this skill.

`parallel-fanout` has a **runnable single-wave path** (plan 760 M1): the
deterministic wave/lane computation lives in the spawn-free
`workflows/parallel-dispatch.lua` seam, and this skill drives it. The seam
COMPUTES (wave, per-lane worktree paths, lane branch names, epic branch name,
fan-in merge order) and HANDS BACK; the model runs the git worktree/branch/merge
ops and spawns the coders. The seam never spawns and never touches
`git worktree`/`git merge` — re-adding a model-spawning primitive is the exact
scope creep that got the old `planar-execute` extracted.

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
5. **Fan out N coders concurrently.** In ONE turn, spawn N `coder` subagents via
   the harness Agent tool, each with `isolation: worktree` pointing at its lane
   worktree, each acquiring its own claim with `planar-agent pull <plan> --role
   coder --worktree <path>`. Each coder heartbeats and RETURNS its diff on its
   lane branch — it does NOT fire a terminal verb. (Axis A isolation is
   non-negotiable: each coder is still a separately spawned subagent.)
6. **Manual fan-in.** After all lanes report done, run the seam's `fan_in` phase
   (`--phase fan_in --args '{"plan_id":<id>,"lanes":[…]}'`) to get the stable
   (task-id-ordered) merge order and teardown list. Merge each lane branch into
   the epic branch in that order with `git merge --no-ff <lane-branch>`, then
   `git worktree remove <worktree>` for each succeeded lane. The orchestrator
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

The fan-in conflict protocol and partial-wave failure/resume are later
milestones (M3–M4); M2 adds staged wave ordering + the wave barrier on top of
M1's single-wave manual fan-in. The fan-in conflict escalation and reconcile-on-
resume paths are NOT yet runnable from this skill.

## Dispatch shape options

Once the strategy is chosen, the orchestrator runs the dispatch-shape gate **nested under the strategy**. The shape describes per-cycle batching, not overall methodology. Under `classic` the operator picks freely from `strict` / `grouped` / `single`. Under `barrel-deferred` and `barrel-bypass` the shape is forced to the matching barrel shape. The `fan-out` shape belongs to the harness-owned `parallel-fanout` strategy, not this skill.

```
  strict           One coder cycle per task; full reviewer per task.
                   [safe; slow] — Recommended for logic changes,
                   multi-file edits, spec/schema changes.

  grouped          Orchestrator picks groupings (typically by milestone
                   or shared file scope); reviewer per group.
                   [balanced] — Useful when tasks are tightly coupled.

  single           All tasks in one coder cycle; one reviewer pass.
                   [tiny features only] — Decomposition is theatre.

  fan-out          (harness-owned — external workflow harness; not
                   run by this skill) N parallel coders + N worktrees,
                   single reviewer at fan-in.

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

`--strict`, `--grouped`, and `--batch` are **not** deprecated — they remain first-class dispatch-shape skips and compose with any strategy that admits them (`classic`, `isolated-sequential`; refused under the forced-shape strategies). Removal of the standalone `--barrel-*` flags is a future cycle's decision, not this cycle's.

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

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
