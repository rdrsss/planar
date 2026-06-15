# Build Spec — Closure & Measurement Subsystem (v0.1)

**Paper working title (placeholder):** *Context Closure as the Unit of Work:
Replication-Minimizing Task Decomposition for Parallel LLM Agents.* Held
loosely — the title hardens only after the results are in (if RQ1 prediction
error dominates, the paper pivots and so does this). It earns its keep now by
giving the drafting something to aim at, not by being final.

**Parent:** `planar-spec-v0.1.md` (the formal/mandate doc). This is the
*implementation* spec. It realizes spec v0.1 §2 (the hypergraph objective),
§4 (static closures), §5 steps 3/5/6 (extractor, measurement rig, baseline),
and §6 (corpus).

**Suggested path in repo:** `docs/research/closure-measurement-build-spec.md`

**Naming:** per the Latin-after-proven convention, no project name is minted
here. Surfaces are named by their verb groups (`planar run *`,
`planar closure *`, `planar groups *`). A name is earned once M3 produces a
result.

**Decomposition intent:** this spec is written for `pl-spec-ingest`. Each
milestone task below is one roadmap bullet → one task row. Touches are
pre-declared per task so the ingestor seeds `task_touch_paths` directly
(dogfooding: the subsystem's own build is the first corpus). Acceptance
criteria are the task's done-signal.

---

## 0. The one thing that must not be confused

There are **four distinct objects** that the phrase "track files and relate
them to work" can mean. The spec keeps them separate on purpose; conflating
them collapses the contribution.

| Object | What it is | Where it lives | Role |
|---|---|---|---|
| **Seed** | declared modify-set: "this task edits these paths" | `task_touch_paths` (exists) | input to extraction |
| **Baseline closure** | declared touches used *as if* they were the closure | `strategy.zig` (exists) | the control the method must beat |
| **Derived closure** $C(t)$ | symbols a task must hold resident, *computed* from the seed by static analysis | `engine/closure/` (M2, new) | **the contribution** |
| **Ground truth** | what `git diff` shows was actually touched | harvest (M1, new) | measurement |

The MVP (M1) ships with derived closure = baseline closure (declared touches).
That is not a shortcut around the contribution; it is the **control arm**.
M2 introduces the *derived* closure and measures the gap between it and the
baseline. If there is no gap, that is itself a finding.

---

## 1. Protected-instrument invariant (non-negotiable)

The measurement subsystem must survive the Lua excision (spec v0.1 §5 step 1)
**by construction**, so M1 can be built *before* the excision and is never
swept out with it.

- The run-record subsystem lives in the **engine + `planar` CLI**, never in
  `planar-execute` and never in the Lua layer. `planar-execute` planner
  modules hold no SQLite handle (their own docblocks); they reach the DB only
  by shelling `planar … --json`. The harness records runs the same way:
  subprocess to `planar run *`.
- **Clean-cut test** (must hold after excision): `planar run *` works, the
  `runs`/`run_events`/`run_touches` tables accept writes, and the harvest
  functions — because none import the Lua layer.
- The instrument is orthogonal to the execution mechanism: a Lua run, a bare
  instrumented-loop run, and a future host-native run are recorded identically.

---

## 2. Pre-registration (FROZEN before M1 runs — do not edit after first run)

Committing this block dated, before any run, is what protects the result from
the garden-of-forking-paths. Exploratory metrics discovered later are labeled
exploratory and are hypotheses for the *next* experiment, not claims in this one.

**Research questions.**

- **RQ1 (gate).** How accurately do declared touches predict actual touches?
  Path-level precision/recall of declared vs. `git diff` ground truth. *No
  arms.* This is the go/no-go: if declared touches are wildly inaccurate, the
  whole program pivots to "static declaration is insufficient, here is the
  error structure" (still a paper, a different one).
- **RQ2 (headline).** Does closure-aware grouping recover parallelism that
  naive eligibility discards, and at what throughput cost? eligibility arm vs.
  grouped arm on parallelism-recovered, wall-clock, total tokens.
- **RQ3 (the tax).** What does co-location cost in failure coupling? failure
  blast-radius, reviewer iterations, conflict rate.
- **RQ4 (exploratory).** How does the strict→eligibility→grouped delta scale
  with the coupling density of the plan?

**Primary metrics** (computed by checked-in SQL over raw tables; never stored
in a `run_metrics` table): wall-clock per plan; total tokens per plan; touch
precision/recall (RQ1); merge-conflict count at fan-in; parallelism recovered
(eligible tasks under grouping − under naive eligibility).

**Hypotheses & falsifiers.** Grouped recovers ≥ X% of serialized parallelism;
if it recovers < Y% *or* inflates conflicts beyond Z, the co-location thesis is
wrong as stated. (Fill X/Y/Z from the pilot's observed spread before M3.)

**Success criterion (arm-independent).** "Done" for a cycle = identical
objective gate across all arms: test suite passes + `zig build` clean +
`zig fmt` clean. Reviewer-agent approval is a *measured variable*
(iterations, request-changes rate), never the definition of success.

**Run discipline.** N = 3–5 reps per cell; arms **interleaved** over calendar
time (not blocked) to convert model drift into noise; paired comparison per
plan; report medians + spread; sign tests over plans rather than p-values from
small N.

---

## 3. Data model

### M1 (exists as drafted): `migrations/00020_runs.{up,down}.sql`

`runs`, `run_events`, `run_touches` — already drafted; land as-is. Key
properties: `run_uid` external id; `config_hash`/`config_json` for cell
comparison; `base_sha` reset point; `run_touches.kind ∈ {declared, actual}`
with declared **snapshotted** (not FK to `task_touch_paths`) so a run is
immutable. No `run_metrics` table, deliberately.

### M2 (new): `migrations/00021_closures.{up,down}.sql`

Snapshot of *derived* closures per task, at symbol granularity, so a run
records which closure model produced its grouping.

```
closures            -- one row per (task, symbol unit) in the derived closure
  id                integer pk
  task_id           integer not null references tasks(id) on delete cascade
  repo_id           integer not null references projects(id) on delete cascade
  path              text    not null          -- file the symbol lives in
  symbol            text    not null          -- qualified symbol name
  role              text    not null          -- 'modify' | 'reference' | 'transitive'
  token_weight      integer not null default 0
  extractor_version text    not null          -- which extractor produced this
  created_at        text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  unique (task_id, repo_id, path, symbol, role, extractor_version)
```

Note `role='transitive'` rows are stored but **excluded from the effective
closure by default** (spec v0.1 §1.2); they exist so the extractor's decisions
are auditable and so promotion experiments are possible without re-extraction.

---

## 4. Verb surface

```
planar run start   --plan <id> --arm <arm> --base-sha <sha> \   <!-- cli-lint-ignore -->
                   --config-hash <h> [--config-json <blob>] --corpus-repo <name>
                   → mints + prints run_uid
planar run event   <run_uid> --kind <k> [--payload <json>]
planar run touch   <run_uid> --task <id> --path <p> --kind <declared|actual>
planar run harvest <run_uid> --task <id> --worktree <path>     -- git diff → actual
planar run finish  <run_uid> --status <completed|aborted|error>
planar run show    <run_uid> [--json]

# M2+
planar closure compute <task-id> [--json]    -- run extractor, write `closures`
planar closure show    <task-id> [--json]

# M3
planar groups recommend <plan-id> [--budget <tokens>] [--json]
                   -- hypergraph partition over closures; emits slices
```

All verbs **READ-ONLY unless named otherwise**; `run *` and `closure compute`
write; `groups recommend` is read-only (computes and reports, like
`recommend-strategy`).

---

## 5. Milestones

### M1 — Measurement rig (MVP, runnable NOW)

Goal: produce the first RQ1 number and validate the pipeline end-to-end using
**only existing decomposition** (`task_touch_paths` + `strategy.zig`). No
extractor, no grouping. Arms: `strict`, `eligibility`.

1. **Land the runs migration.** Drop `00020_runs.{up,down}.sql` into
   `migrations/`; verify codegen embeds it and schema version → 20.
   *Touches:* `migrations/00020_runs.up.sql`, `migrations/00020_runs.down.sql`.
   *Accept:* `planar health` clean on a fresh DB; `zig build test` green.

2. **`engine/runs/` module.** Run lifecycle (start/event/touch/finish/show)
   and the harvest helper (`git diff --name-only` against a worktree branch →
   `run_touches kind='actual'`). DB-side; owns the SQLite writes.
   *Touches:* `src/engine/runs/runs.zig`, `src/engine/runs/harvest.zig`.
   *Accept:* unit tests with a synthetic git worktree fixture; harvest returns
   the expected path set.

3. **`planar run *` verb group.** Wire the verbs as siblings to existing
   handlers; enum-validate `arm`/`status`/`kind` at the parse layer.
   *Touches:* `src/cmd/planar/handlers/run/cmd.zig`,
   `src/cmd/planar/handlers/run/{start,event,touch,harvest,finish,show}.zig`.
   *Accept:* integration test drives a full run lifecycle via the CLI and reads
   it back with `run show --json`.

4. **Clean-slate run ritual** in the harness: reset to `base_sha` in a fresh
   worktree, run, harvest, finish, tear down. Harness reaches the DB only by
   shelling `planar run *` (preserves the protected-instrument invariant).
   *Touches:* `src/cmd/planar-execute/` (new run-ritual module; NO db import).
   *Accept:* a dummy slice flows end-to-end into clean joined records.

5. **Declared-touch snapshot at run start.** On `run start`, snapshot the
   plan's current `task_touch_paths` into `run_touches kind='declared'`.
   *Touches:* `src/engine/runs/runs.zig`.
   *Accept:* re-declaring touches after a run does not alter that run's
   declared snapshot.

6. **`metrics/` query directory.** Checked-in SQL for the primary metrics;
   RQ1 precision/recall is the self-join from the schema doc.
   *Touches:* `metrics/rq1_touch_accuracy.sql`, `metrics/tokens_per_plan.sql`,
   `metrics/wallclock_per_plan.sql`, `metrics/conflicts.sql`, `metrics/README.md`.
   *Accept:* each query runs against a pilot DB and returns sane shape.

7. **Pilot.** One plan, one rep, `strict` + `eligibility` only. Produces the
   first RQ1 number. **Hard go/no-go gate** for M2/M3.
   *Touches:* `docs/research/pilot-notes.md`.
   *Accept:* RQ1 precision/recall computed; pipeline produces no orphaned or
   unjoinable records.

**M1 exit:** the rig works, RQ1 has a number, and you can start testing the
measurement loop on real plans today. The eligibility arm here uses declared
touches — the baseline M2 must beat.

### M2 — Derived closure extractor (the contribution; agent picks up after M1)

Goal: replace declared-touch proxy with a *computed* symbol-level closure.
This is spec v0.1 §5 step 3, the long pole — concentrate engineering risk here.

1. **Vendor tree-sitter.** Bring the tree-sitter C runtime + target-language
   grammar into `vendor/` matching the `vendor/{sqlite,lua}` pattern (compiled
   by `build.zig`, no system dep). Start with one language present in the corpus.
   *Touches:* `vendor/tree-sitter/`, `vendor/manifest.zon`, `build.zig`.
   *Accept:* `zig build` links it; a smoke test parses a fixture file to a tree.

2. **Symbol resolution.** From a seed path (a `task_touch_paths` row), resolve
   the file to its declarations; identify the modify-set symbols.
   *Touches:* `src/engine/closure/symbols.zig`.
   *Accept:* given a fixture, returns the expected qualified symbol set.

3. **Reference-edge walk + role partition.** For each modify symbol, pull the
   *interfaces* (signatures/types) of referenced symbols (role `reference`);
   mark deeper hops `transitive` (excluded from effective closure by default).
   *Touches:* `src/engine/closure/walk.zig`.
   *Accept:* effective closure = `modify ∪ interfaces(reference)`; transitive
   excluded; matches a hand-verified fixture closure.

4. **Token weighting.** `w(u)` per unit (raw token count for MVP; OQ-3 parks
   role-adjusted weighting).
   *Touches:* `src/engine/closure/weight.zig`.
   *Accept:* weights sum correctly; deterministic across runs.

5. **`closures` migration + `planar closure compute|show`.**
   *Touches:* `migrations/00021_closures.up.sql`,
   `migrations/00021_closures.down.sql`,
   `src/cmd/planar/handlers/closure/cmd.zig`,
   `src/engine/closure/store.zig`.
   *Accept:* `closure compute <task>` writes rows; `closure show --json` reads
   them; `extractor_version` recorded.

6. **Re-run eligibility with derived closures.** Add an eligibility variant
   that reads `closures` instead of `task_touch_paths`. Measure derived-vs-
   declared overlap divergence (the gap that justifies the extractor).
   *Touches:* `src/engine/planning/strategy.zig` (closure-source switch).
   *Accept:* both sources runnable behind a flag; divergence query returns a
   number.

**M2 exit:** closures are *computed*, not asserted; the baseline-vs-derived gap
is measured. RQ1 can now be reported at symbol granularity (harvest may still
be path-level — note as precision ceiling per OQ-1).

### M3 — Grouping (headline arm; after M2)

Goal: form slices by minimizing closure replication under the window budget.

1. **Greedy overlap-merge first pass.** Sanity-check grouping before reaching
   for a solver: merge tasks by descending closure overlap until the unioned
   closure hits budget `B`.
   *Touches:* `src/engine/grouping/greedy.zig`.
   *Accept:* respects budget; never groups across a dependency violation.

2. **`planar groups recommend` verb** (read-only sibling to
   `recommend-strategy`); emits slices + per-slice unioned closure + cost.
   *Touches:* `src/cmd/planar/handlers/plan/recommend_groups.zig`,
   `src/cmd/planar/handlers/plan/cmd.zig` (register).
   *Accept:* integration test asserts slice membership + budget compliance on a
   fixture plan.

3. **KaHyPar binding** (off-the-shelf partitioner; spec v0.1 §3 — do NOT write
   a new partitioner). Hyperedge per context unit; λ−1 connectivity objective;
   window as balance constraint.
   *Touches:* `vendor/kahypar/`, `vendor/manifest.zon`, `build.zig`,
   `src/engine/grouping/kahypar.zig`.
   *Accept:* produces a partition with cost ≤ the greedy pass on the same input.

4. **Grouped arm + full corpus runs.** Three arms × corpus × N reps,
   interleaved, against a spend ledger with a pre-committed ceiling.
   *Touches:* `docs/research/results.md`, `metrics/parallelism_recovered.sql`.
   *Accept:* RQ2/RQ3 numbers computed; paired per-plan deltas reported.

---

## 6. Decisions to lock before M1 (resolved defaults; override deliberately)

- **D1 — harvest granularity.** Path-level for the MVP ground truth (cheap,
  ships now); symbol-level harvest deferred to a refinement gated on whether
  path-level precision is already adequate. *(Resolved: path-level MVP.)*
- **D2 — `run_touches.task_id` integrity.** Plain integer, not FK-cascade: a
  run is immutable historical evidence; deleting a task must not erase what it
  touched. *(Resolved: plain int.)*
- **D3 — `run_events` independence.** Standalone from `agent_activity` so
  operational/schema changes can't perturb a recorded benchmark.
  *(Resolved: standalone.)*
- **D4 — closure source for eligibility.** Behind a flag so declared (baseline)
  and derived (M2) are both runnable; the comparison is the experiment.
  *(Resolved: flag.)*

Record each as a `planar decision` on the anchor plan at ingest so the agent
treats them as locked.

---

## 7. Corpus (spec v0.1 §6)

Primary: `git-fleet` (a real decomposed feature, not synthetic — authenticity is
part of the claim). Secondary: Planar's own backlog (self-hosted, disclosed).
Pick plans that **vary in coupling density** — one heavily coupled, one
embarrassingly parallel, one mixed — because RQ4's finding is how the arm-delta
scales with coupling, not a single average.

---

## 8. Out of scope (v0.1)

Model-inferred closures (spec v0.1 §4 — the *second* experiment); host-agnostic
interchange format (spec v0.1 §7 — downstream of a result); the Lua excision
itself (separate work; this spec only requires excision-*safety*, which the
protected-instrument invariant guarantees); role-adjusted token weighting
(OQ-3); symbol-level harvest (refinement gated on D1).

---

## 9. Critical path

`M1.1 → M1.2 → M1.3 → M1.4/5 → M1.6 → M1.7 (pilot gate)` then
`M2.1 → M2.2/3 → M2.4/5 → M2.6` then `M3`.

**Start the first keystroke at M1.1.** The rig is built and proven on the two
existing arms before the extractor (M2) or grouping (M3) exist — so when the
headline feature lands, it aims at an instrument that already works.
