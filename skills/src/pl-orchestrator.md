---
slug: pl-orchestrator
description: "Run the orchestrator over a goal, anchor plan, or task list — manage the full feature lifecycle (planning, ingestion, execution, propagation, archive) with reviewer iteration cap and user gates at each phase boundary."
source: agents/orchestrator.md
model_tier: large
vendor:
  claude:
    argument_hint: "<goal|plan-id|task-id> [<task-id>...] [--propagate] [--archive] [--strategy <name>] [--strict | --grouped | --batch <ids>]"
    invocation_examples: |
      /orchestrator <goal>                          # start from scratch: plan → wait → ingest → wait → execute
      /orchestrator <anchor-plan-id>                # resume from current anchor plan status
      /orchestrator <task-id> [<task-id>...]        # execute specific tasks (Phase 3 only)
      /orchestrator <goal> --propagate              # plan → ingest → execute → propagate
      /orchestrator <anchor-plan-id> --archive      # execute → mark done → archive FS tree
      /orchestrator <plan-id> --strategy classic              # explicit continuity — coder in pwd, current branch, sequential
      /orchestrator <plan-id> --strategy isolated-sequential  # coder in a worktree on an epic-child branch; sequential
      /orchestrator <plan-id> --strategy parallel-fanout      # fan out to N coders + N worktrees; reviewer at fan-in
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

## Phase Behavior

The orchestrator selects phases based on the anchor plan's current `status`:

1. **Planning (Phase 1)** — anchor plan in `draft` with no workbench artifacts: invokes `pl-spec-draft "<goal>"`, surfaces the drafted artifacts to the user, and **waits for explicit review** before proceeding. Does not auto-advance.

2. **Ingestion (Phase 2)** — anchor plan in `draft` with workbench artifacts present: invokes `pl-spec-ingest <plan>` in preview mode (no `--apply`), presents the diff to the user, and **waits for explicit confirmation** before running `--apply`. Never auto-applies.

3. **Execution (Phase 3)** — anchor plan `active` or `paused`: first reads claim-aware state with `planar plan next <plan>` (operator-side) or `planar-agent peek <plan>` (agent-side dry-run for explicit task IDs), excludes active unexpired claims, and surfaces stale claims before dispatch. Phase 3 then runs **two gates in order** before any coder runs: the **strategy gate** (new — see below), which picks the overall methodology for the plan (`classic` / `isolated-sequential` / `parallel-fanout` / `barrel-deferred` / `barrel-bypass`), followed by the **dispatch-shape gate** (existing — `strict` / `grouped` / `single`), which picks the per-cycle batching nested under the chosen strategy. Both gates wait for explicit operator confirmation; both can be pre-committed via flags (`--strategy <name>` / `--strict` / `--grouped` / `--batch`). Before dispatching each cycle the orchestrator acquires the lease atomically via `planar-agent pull <plan>` (or `planar-agent claim --entity task:<id>` for hand-picked targets) and records the returned `claim_token` in the dispatch entry. The dispatch heuristic is **parallelism-aware**: under the `parallel-fanout` strategy it consults the `entity_links` graph + task touches metadata to identify the parallel-eligible subset and fans out N coders into N worktrees per [Worktrees](../../agents/methodology.md#worktrees); under all other strategies cycles run sequentially. After the coder reports done, the orchestrator runs **Phase 3.5 — test-coder dispatch** (see below): consults `planar test-spec status <plan> --json` and, when the cycle's dispatched slugs intersect the JSON's `uncovered_task_slugs`, dispatches `pl-test-coder`. The output (coder diff alone or the union of coder + test-coder diffs) is routed through `pl-reviewer`. The cycle terminates via one of `planar-agent complete` / `fail` / `release` / `block` (atomic — flips both claim status and task status in a single transaction). Enforces the 5-iteration cap per coder/reviewer cycle (the test-coder cycle has its own cap, default 2), and surfaces escalations (open questions, aborts, ship-with-caveats, failure-surfaced).

   **Strategy gate (first thing Phase 3 does, after reading claim state).** The orchestrator runs the recommendation algorithm against the plan — see [`agents/methodology.md` § Recommendation algorithm](../../agents/methodology.md#recommendation-algorithm) for the rules (mechanical/docs/single-verb → `barrel-bypass`; multi-milestone roadmap with ≤1 parallel-eligible per milestone → `barrel-deferred`; ≥3 tasks with ≥2 parallel-eligible → `parallel-fanout`; 2–3 tasks none parallel-eligible → `isolated-sequential`; single-task → `classic`; otherwise stickiness then `classic`). It then surfaces:

   - the recommended strategy,
   - a one-line rationale (e.g. "2-task plan, neither parallel-eligible"),
   - the full menu of the five named strategies with one-line trade-offs (see [Strategy menu](#strategy-menu) below),
   - the `--strategy custom` escape hatch for axis-by-axis overrides.

   The orchestrator **waits for explicit operator confirmation** before doing any further Phase 3 work (no claim acquisition, no dispatch-shape proposal, no coder dispatch). Auto-defaulting without confirmation is not supported: the recommendation never silently turns into an action.

   The strategy gate is skipped only when `--strategy <name>` (or `--strategy custom --isolation X --branch-model Y ...`) was supplied at invocation. The dispatch-shape gate then runs nested under the chosen strategy, constrained by it: `parallel-fanout` forces the fan-out shape; `barrel-bypass` forces the barrel-bypass shape; `barrel-deferred` forces the barrel-deferred shape; `classic` and `isolated-sequential` keep the full strict / grouped / single menu. The dispatch-shape gate is itself bypassed only when `--strict`, `--grouped`, `--batch`, or a `--barrel-*` standalone flag was supplied (the standalone barrel-* flags are soft-deprecated — see [Aliases and deprecations](#aliases-and-deprecations)).

   **Strategy stickiness is deferred.** The recommendation algorithm's rule 6 ("if the last dispatch used non-default strategy S, recommend S") rides on `agent_actions.metadata` JSON which does not yet have the column or CLI plumbing to read/write it (task 2939 follow-up). Until that lands, the orchestrator does **not** auto-recall the prior cycle's strategy — it re-derives the recommendation from plan shape each cycle. The operator confirms each time, so the only loss is one-keypress repetition. Do not assume `agent_actions.metadata` writes happen today; they do not.

   **Phase 3.5 outcomes:**
   - `expanded` → test-coder diff staged alongside coder's; reviewer sees the union.
   - `no-expansion-needed` → reviewer sees the coder's diff alone.
   - `failure-surfaced` → escalates to the user with the test-coder's report (failing tests classified as `test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`); reviewer NOT dispatched until the user resolves.
   - `abort` → escalates; cycle halts.

4. **Propagation (Phase 4, optional)** — invokes `pl-ext-propagate <plan>` against the registered external system when the user requests `--propagate` or confirms interactively. Never propagates silently.

5. **Archive (Phase 5, optional)** — marks anchor plan `done` and invokes `planar workbench archive <anchor>` when the user requests `--archive` or confirms interactively. Never archives automatically.

## User Gates

- Between Phase 1 and Phase 2: user must review artifacts.
- Between Phase 2 preview and `--apply`: user must confirm the diff.
- Phase 3 **strategy**: user picks (or confirms the recommendation of) one of the five named strategies (`classic` / `isolated-sequential` / `parallel-fanout` / `barrel-deferred` / `barrel-bypass`), or supplies `--strategy custom` with per-axis flags. This gate runs **first** in Phase 3, before claim acquisition or dispatch-shape selection. Skipped only when `--strategy <name>` was supplied at invocation.
- Phase 3 **dispatch shape**: user picks one of the dispatch shapes (strict/grouped/single — or the legacy six-shape menu when a standalone barrel-* flag is in play) before any coder runs. Runs nested under the chosen strategy and is constrained by it (`parallel-fanout`/`barrel-deferred`/`barrel-bypass` force the corresponding shape). Skipped when `--strict`/`--grouped`/`--batch`/`--barrel-grouped`/`--barrel-deferred`/`--barrel-bypass` was supplied.
- Phase 3 claim conflicts: active unexpired claims are not silently bypassed. Stale claims require reconciliation or explicit force-takeover before the work is considered available.
- Phase 3.5 `failure-surfaced` outcome: when a test the test-coder authored fails on first run, user must resolve (fix the test or fix the code) before the reviewer is dispatched. The orchestrator never decides which side is wrong.
- Phase 4: user must request propagation.
- Phase 5: user must request archive.

These gates exist to prevent silent side effects on spec/task creation and FS cleanup.

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

## Brief composition

The dispatcher's brief is the input the coder runs on. Sloppy briefs are a dispatcher problem to prevent, not a coder problem to recover from. Every coder brief composed by the orchestrator MUST:

- **Cite spec section paths, not paraphrased spec content.** The brief is a pointer to the workbench tech-spec; a paraphrase loses load-bearing detail and silently becomes the coder's source of truth.
- **List task IDs explicitly.** The cycle's scope is the enumerated task list. Task IDs are the contract the reviewer compares the diff against.
- **List claim tokens explicitly.** Claim tokens are the synchronization contract. The reviewer uses them to verify the diff stayed inside leased scope, and interrupted sessions use them for resume/reconcile.
- **Note locked decisions inline.** Patterns like `Q47 = editor markers` or "ADR-0012 forbids new global state here" go in the brief verbatim so the coder does not re-litigate them.
- **Specify the gates the coder must run.** Default Zig gates: `make fmt-check`, `make build`, `make test`, `make test-integration` (run twice, back-to-back), plus `planar skills render --check` against an out-of-tree staging dir when skill/agent surfaces are touched and any remaining relevant validators.
- **Specify the report shape.** Word ceiling and the required sections per [`agents/coder.md` §Work-complete report template](../../agents/coder.md#work-complete-report-template).
- **Pose the problem; do not include the solution.** State the invariant, the constraint, and the acceptance signal. Let the coder design the implementation.
- **Cite test-spec section paths when dispatched tasks have `verifies` edges to test-spec scenarios (plan 277).** List the cited scenario IDs explicitly so the reviewer can compare the diff against them. Skip the test-spec reference when no scenarios are cited.

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

Before asking the operator to confirm the strategy gate, the orchestrator surfaces all five named strategies with a one-line trade-off each, plus the `--strategy custom` escape hatch. Strategy answers "what is the overall methodology for this plan?" — dispatch shape (next section) answers "within that strategy, how do I batch *this cycle's* work?"

```
  classic                Coder runs in operator's pwd on the current
                         branch. Sequential cycles, reviewer per cycle,
                         test-coder per cycle. No worktrees, no epic
                         branch, no parallelism.
                         [continuity guarantee — today's behavior bit-for-bit]
                         Recommended for: single-task changes, small plans,
                         high-stakes invariant-touching work.

  isolated-sequential    Coder runs in a dedicated worktree on a child
                         branch off an epic branch. Sequential cycles,
                         reviewer per cycle.
                         [pwd hygiene + per-task rollback] — Recommended
                         for: multi-task plans, cross-cutting work.

  parallel-fanout        Fan out to N parallel coders on the parallel-
                         eligible subset; each in its own worktree on its
                         own child branch; single reviewer pass at fan-in.
                         [throughput + integrated review] — Recommended
                         for: plans with ≥3 tasks and ≥2 parallel-eligible.
                         Refused for: schema-migration-heavy plans,
                         singleton-file editing plans.

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

The five named strategies are bundles of the five underlying axes (`isolation`, `branch_model`, `concurrency`, `reviewer_cadence`, `test_coder_cadence`); the axes and the named-bundle table live in [`agents/methodology.md` § Orchestration strategies](../../agents/methodology.md#orchestration-strategies). The orchestrator refuses the invalid axis combinations listed there (e.g. `concurrency=fan-out` with `isolation=in-pwd`) with a diagnostic before any dispatch runs.

### `classic` — the continuity guarantee

`classic` is the explicit continuity default, not a legacy or deprecated mode. When the operator selects (or accepts the recommendation of) `classic`, the orchestrator's behavior matches today's bit-for-bit:

- **Explicitly skips:** worktree creation, epic branch creation, child-branch creation, fan-in merge, any parallel dispatch.
- **Dispatches:** the coder against the operator's pwd on whatever branch is currently checked out. No `isolation: "worktree"` flag, no `--worktree <path>` on `planar-agent pull`.
- **Reviewer:** runs per cycle, per the existing `agents/methodology.md` reviewer dispatch profile.
- **Test-coder:** runs per cycle when uncovered slugs intersect the cycle's slugs, per Phase 3.5.

The promise: introducing the strategy menu does not require existing operators to learn a new flow to keep working as they do. Pick `classic`, get today's behavior. See [`agents/methodology.md` § Continuity guarantee: `classic`](../../agents/methodology.md#continuity-guarantee-classic) for the framing.

### `isolated-sequential` — per-cycle worktree on an epic-child branch

Under `isolated-sequential` the orchestrator drives a strict per-cycle worktree lifecycle so the operator's main checkout stays on master throughout the plan. The authoritative path/branch/topology conventions live in [`agents/methodology.md` § Worktrees](../../agents/methodology.md#worktrees) — do not re-derive them. This subsection describes the *orchestrator's actions* per cycle.

The shape of the topology, repeated only for orientation: main checkout stays on master; the epic branch `epic/<plan-slug>` lives in `<repo>/.worktrees/epic/<plan-slug>/` and persists for the plan's duration; each cycle's child branch `cycle/<plan-slug>/<task-slug>` lives in `<repo>/.worktrees/cycle/<plan-slug>/<task-slug>/` and is removed after reviewer approval. `<repo>` is the *task's owning repo*, not the operator's cwd repo. `epic/<plan-slug>` and `cycle/<plan-slug>/<task-slug>` are disjoint top-level branch namespaces — never the bare `<plan-slug>` form (see methodology § Branch scheme for the ref-hierarchy collision this avoids).

#### Per-cycle worktree creation

Two steps. The first runs once per plan; the second runs every cycle.

```bash
# 1. First dispatch of any task in the plan — create the epic branch + worktree
#    if epic/<plan-slug> does not yet exist. Skip on subsequent cycles.
if ! git -C <repo> show-ref --quiet refs/heads/epic/<plan-slug>; then
  git -C <repo> branch epic/<plan-slug> master
  git -C <repo> worktree add <repo>/.worktrees/epic/<plan-slug>/ epic/<plan-slug>
fi

# 2. Every cycle — create the per-cycle child branch + worktree off the epic.
git -C <repo> worktree add -b cycle/<plan-slug>/<task-slug> \
  <repo>/.worktrees/cycle/<plan-slug>/<task-slug>/ epic/<plan-slug>
```

The `-b` form on step 2 creates the child branch and the worktree in one call; the child is cut from `epic/<plan-slug>`, not master, so the cycle inherits any prior cycles that have already fanned in. The main checkout is never touched by either step. Apply the `.git/info/exclude` ritual described in [`.git/info/exclude` ritual](#gitinfoexclude-ritual) below on the first worktree creation per clone.

#### Coder dispatch under `isolated-sequential`

The orchestrator dispatches the coder Agent with the cycle worktree as the isolation boundary:

- **Agent tool call:** pass `isolation: "worktree"` and point the isolation path at `<repo>/.worktrees/cycle/<plan-slug>/<task-slug>/`. The Agent tool understands `isolation: "worktree"` as the contract that the coder must operate inside the given directory and must not write outside it.
- **Claim wiring in the brief:** the brief instructs the coder to invoke `planar-agent pull <plan> --role coder --worktree <cycle-worktree-path>` (or `claim --entity task:<id> --worktree <path>` for hand-picked targets). The `--worktree` flag persists the path on `agent_work_claims.worktree_path` so resume/handoff can recover it.
- **First-action directive:** the brief MUST tell the coder, as its first action before any other work, to `cd <cycle-worktree-path>`. This is defensive — the Agent's cwd at spawn time may not match the isolation path, and a stray edit in the main checkout would violate the topology invariant. State the directive verbatim; do not paraphrase.
- **Everything else about brief composition is unchanged.** Spec citations, task IDs, claim tokens, locked decisions, gates, report shape, test-spec references — see [Brief composition](#brief-composition) for the full contract. The `isolated-sequential` additions are the isolation parameter, the `--worktree` flag, and the cd directive.

#### Fan-in merge (post-coder-terminal-complete)

After the coder reports terminal-complete and the orchestrator confirms the claim status flipped via the atomic `planar-agent complete`, the orchestrator runs the fan-in inside the *epic* worktree:

```bash
# cd into the EPIC worktree — NOT the main checkout, NOT the cycle worktree.
cd <repo>/.worktrees/epic/<plan-slug>/

# Optional: sync master in first when master has advanced since the last sync.
# Recommended; honest trade-off — this adds one merge commit per sync to the
# epic's history. Skip when master has not advanced.
git fetch origin master
git merge --no-ff origin/master -m "Sync epic/<plan-slug> with master @ <sha>"

# Fan-in: merge the cycle child into the epic. --no-ff preserves the per-cycle
# history shape (which matters more under parallel-fanout in M8/M9, but the
# convention is locked here).
git merge --no-ff cycle/<plan-slug>/<task-slug> \
  -m "Plan <id> <milestone> fan-in: <one-line summary>"
```

Outcomes:

- **Clean merge** → proceed to reviewer dispatch (per the `isolated-sequential` per-cycle reviewer cadence) and then to cleanup below.
- **Conflict** → open a question via `planar question add --plan <plan> ...` capturing the conflict-marker output, halt this branch's fan-in, and **leave both worktrees on disk** (cycle worktree + epic worktree) for operator resolution. Do not attempt to classify "trivial vs. semantic" — that's an operator judgement. See [`agents/methodology.md` § Conflict resolution at fan-in](../../agents/methodology.md#conflict-resolution-at-fan-in) for the full taxonomy.

The main checkout is not involved at any point in fan-in. This is what preserves the "operator pwd stays clean" promise.

#### Post-reviewer-approval cleanup

Cleanup is **post-success only** — never before reviewer approval, so a failed cycle leaves recoverable artifacts on disk. After the reviewer returns `approve` (or `approve` with caveats — caveats are filed as new tasks per [Iteration 5 contract](#iteration-5-contract); they do not block cleanup):

```bash
# Remove the cycle worktree. Safe to run from the main checkout or anywhere
# else — the operation targets the worktree by path.
git -C <repo> worktree remove <repo>/.worktrees/cycle/<plan-slug>/<task-slug>/

# Force-delete the cycle branch. -D (not -d) because the branch is merged
# into epic/<plan-slug> but NOT into master, and `git branch -d` checks
# merged-into-HEAD which here is master.
git -C <repo> branch -D cycle/<plan-slug>/<task-slug>
```

The **epic worktree and `epic/<plan-slug>` branch persist** through cleanup. They are not removed per cycle. Removal happens only after the operator merges `epic/<plan-slug>` into master (operator-driven, surfaced as "epic/<plan-slug> is N commits ahead of master, reviewer-approved, ready for PR" — the orchestrator does not run the epic→master merge itself).

#### `.git/info/exclude` ritual

On the first worktree creation per clone, the orchestrator checks whether `.worktrees/` is already listed in `.git/info/exclude`. If not, it appends it:

```bash
if ! grep -qxF '.worktrees/' <repo>/.git/info/exclude 2>/dev/null; then
  echo '.worktrees/' >> <repo>/.git/info/exclude
fi
```

`.git/info/exclude` is per-clone state and does not propagate via git. After the append, the orchestrator surfaces a one-line hint to the operator:

```
note: appended .worktrees/ to .git/info/exclude (per-clone, no commit).
      For a shared repo, consider committing a repo-level .gitignore
      entry so other clones get the same exclusion.
```

The hint is informational, not blocking. The orchestrator does not commit the `.gitignore` entry itself — that's an operator choice (repo conventions vary on whether `.worktrees/` belongs in `.gitignore` or stays per-clone).

## Dispatch shape options

Once the strategy is chosen, the orchestrator runs the dispatch-shape gate **nested under the strategy**. The shape describes per-cycle batching, not overall methodology. Under `classic` and `isolated-sequential` the operator picks freely from `strict` / `grouped` / `single`. Under `parallel-fanout` the shape is forced to `fan-out`. Under `barrel-deferred` and `barrel-bypass` the shape is forced to the matching barrel shape.

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
                   coders + N worktrees, single reviewer at fan-in.

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

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
