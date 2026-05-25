---
name: pl-orchestrator
description: Run the orchestrator over a goal, anchor plan, or task list — manage the full feature lifecycle (planning, ingestion, execution, propagation, archive) with reviewer iteration cap and user gates at each phase boundary.
model: gpt-5
source: agents/orchestrator.md
---

# Orchestrator (Codex)

Codex skill surface for the vendor-neutral `orchestrator` agent. See [`agents/orchestrator.md`](../../agents/orchestrator.md) for the full role spec, phase descriptions, and phase-selection logic. See [`agents/methodology.md`](../../agents/methodology.md) for the iteration cap, reviewer decisions, concurrency rules, and escalation paths.

## Phase Behavior

The orchestrator selects phases based on the anchor plan's current `status`:

1. **Planning (Phase 1)** — anchor plan in `draft` with no workbench artifacts: invokes `pl-spec-draft "<goal>"`, surfaces the drafted artifacts to the user, and **waits for explicit review** before proceeding. Does not auto-advance.

2. **Ingestion (Phase 2)** — anchor plan in `draft` with workbench artifacts present: invokes `pl-spec-ingest <plan>` in preview mode (no `--apply`), presents the diff to the user, and **waits for explicit confirmation** before running `--apply`. Never auto-applies.

3. **Execution (Phase 3)** — anchor plan `active` or `paused`: first reads claim-aware state with `planar plan next <plan>` (or equivalent for explicit task IDs), excludes active unexpired claims, and surfaces stale claims before dispatch. It then proposes a dispatch shape (`strict` / `grouped` / `single`) per [Dispatch Granularity](../../agents/methodology.md#dispatch-granularity) and **waits for explicit confirmation** before any coder runs. Before dispatching each cycle it acquires `planar agent claim` leases and records claim tokens in the dispatch entry. After the coder reports done, the orchestrator runs **Phase 3.5 — test-coder dispatch** (see below): consults `planar test-spec status <plan> --json` and, when the cycle's dispatched slugs intersect the JSON's `uncovered_task_slugs`, dispatches `pl-test-coder`. The output (coder diff alone or the union of coder + test-coder diffs) is routed through `pl-reviewer`. Enforces the 5-iteration cap per coder/reviewer cycle (the test-coder cycle has its own cap, default 2), and surfaces escalations (open questions, aborts, ship-with-caveats, failure-surfaced). The dispatch-shape gate is bypassed only when `--strict`, `--grouped`, or `--batch` was supplied at invocation.

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
- Phase 3 dispatch shape: user picks one of the six shapes (strict/grouped/single/barrel-grouped/barrel-deferred/barrel-bypass) before any coder runs (unless `--strict`/`--grouped`/`--batch`/`--barrel-grouped`/`--barrel-deferred`/`--barrel-bypass` was supplied).
- Phase 3 claim conflicts: active unexpired claims are not silently bypassed. Stale claims require reconciliation or explicit force-takeover before the work is considered available.
- Phase 3.5 `failure-surfaced` outcome: when a test the test-coder authored fails on first run, user must resolve (fix the test or fix the code) before the reviewer is dispatched. The orchestrator never decides which side is wrong.
- Phase 4: user must request propagation.
- Phase 5: user must request archive.

These gates exist to prevent silent side effects on spec/task creation and FS cleanup.

## Brief composition

The dispatcher's brief is the input the coder runs on. Sloppy briefs are a dispatcher problem to prevent, not a coder problem to recover from. Every coder brief composed by the orchestrator MUST:

- **Cite spec section paths, not paraphrased spec content.** The brief is a pointer to the workbench tech-spec; a paraphrase loses load-bearing detail and silently becomes the coder's source of truth.
- **List task IDs explicitly.** The cycle's scope is the enumerated task list. Task IDs are the contract the reviewer compares the diff against.
- **List claim tokens explicitly.** Claim tokens are the synchronization contract. The reviewer uses them to verify the diff stayed inside leased scope, and interrupted sessions use them for resume/reconcile.
- **Note locked decisions inline.** Patterns like `Q47 = editor markers` or "ADR-0012 forbids new global state here" go in the brief verbatim so the coder does not re-litigate them.
- **Specify the gates the coder must run.** Default Go gates: `gofmt`, `go vet`, `go build`, `go test`, two-run integration confirmation, plus `make render-check` when skill/agent surfaces are touched and any remaining relevant validators.
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

## Dispatch mode options

Before asking the user to select a dispatch mode, the orchestrator presents all six shapes with a one-line trade-off each:

```
  strict           One coder cycle per task; full reviewer per task.
                   [safe; slow] — Recommended for logic changes,
                   multi-file edits, spec/schema changes.

  grouped          Orchestrator picks groupings (typically by milestone
                   or shared file scope); reviewer per group.
                   [balanced] — Useful when tasks are tightly coupled.

  single           All tasks in one coder cycle; one reviewer pass.
                   [tiny features only] — Decomposition is theatre.

  barrel-grouped   Alias for grouped with the milestone heuristic
                   locked in. Reviewer per group.
                   [throughput + per-group review]

  barrel-deferred  Coder cycles run back-to-back; reviewer fires at
                   the configured boundary (milestone default;
                   --barrel-deferred-at plan for once-per-plan).
                   [throughput + late review safety net]

  barrel-bypass    No reviewer dispatch at all. Quality gates (gofmt,
                   vet, build, test, integration-twice, parity
                   validators) ARE the entire review signal.
                   [maximum throughput; trust the gates]

  When in doubt: use --strict.
```

The three `barrel-*` shapes are documented in detail in [`agents/methodology.md` §Barrel modes](../../agents/methodology.md#barrel-modes). They are siblings of `strict`/`grouped`/`single` on the same gate; the operator picks one shape per `pl-orchestrator` invocation.

Phase 3.5 (test-coder dispatch) fires across all barrel modes when uncovered slugs intersect the cycle's slugs. `barrel-bypass` bypasses the reviewer, not the coverage gate.

See `agents/methodology.md` § "Dispatch mode selection" for the full
inline-vs-strict rule.

## Vendor Notes

- Installed into `~/.codex/skills/pl-orchestrator` from `~/.planar/codex-skills/pl-orchestrator`.
- Dispatches to the host vendor's Planar skill surfaces by default; cross-vendor dispatch uses the destination vendor's invocation surface.
