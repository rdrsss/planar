# Run records & the measurement-rig separation boundary

**Status:** design · companion to `migrations/00020_runs.{up,down}.sql`
**Serves:** the vertical-slice decomposition experiment (strict / eligibility / grouped) and Planar spec v0.1 §5 step 5 ("measurement rig — must survive the §1 excision, not be swept out with it").

This document does two things: specifies the run-record schema, and pins
the architectural boundary that lets the Lua execution layer be excised
(spec §5 step 1) without amputating the benchmark instrument.

---

## 1. The separation hazard, and why the schema dissolves it

The spec calls for a clean cut: remove the Lua execution engine, archive
it as an independent toy. The hazard flagged during design: Planar's
token-counting and run-record machinery is currently *adjacent* to that
Lua layer (the `planar-execute` journal, `ctx.eligible`, the brief
compiler). A naive cut amputates the instrument along with the engine.

The repo already encodes the resolution. Every `planar-execute` planner
module (`state.zig`, `schema.zig`, `brief.zig`) carries the same invariant
in its docblock:

> This module holds NO SQLite handle and imports NO db/engine/runtime
> module. Every state read is a subprocess call followed by JSON parse.

The harness reaches the database **only** by shelling `planar … --json`.
It cannot write run records directly because it has no DB handle — and
that is the feature, not the limitation.

**Therefore the run-record subsystem lives in the engine, reached by a CLI
verb group, never in the Lua layer:**

```
src/engine/runs/                  -- run lifecycle + harvest logic (DB-side)
src/cmd/planar/handlers/run/      -- `planar run *` verbs (record/event/harvest/show)
migrations/00020_runs.*.sql       -- the schema
```

The harness — Lua workflow, bare instrumented loop, or a future
host-native driver — emits records the same way it already reads state:
by subprocess.

```
planar run start   --plan <id> --arm <arm> --base-sha <sha> \   <!-- cli-lint-ignore -->
                   --config-hash <h> --config-json <blob> --corpus-repo <name>
                   → mints run_uid, inserts the runs row, prints run_uid

planar run event   <run_uid> --kind token_sample --payload '{"in":…,"out":…}'
planar run touch   <run_uid> --task <id> --path <p> --kind declared
planar run harvest <run_uid> --task <id> --worktree <path>   -- runs git diff,
                                                                writes kind=actual
planar run finish  <run_uid> --status completed
```

### The clean-cut test

After the Lua excision, the following must still hold — and do, because
none of these import the Lua layer:

- `planar run *` verbs work.
- The `runs` / `run_events` / `run_touches` tables exist and accept writes.
- The harvest (git-diff → `kind=actual`) functions.

The instrument is **orthogonal to the execution mechanism**. You can
measure a Lua-driven run, a bare-loop run, and a host-native run with the
identical recording surface. That orthogonality is the whole point of
putting the subsystem behind the CLI seam.

---

## 2. Schema rationale

Three tables; no fourth. The omission is deliberate (see §3).

### `runs` — the experimental unit

One row per `(plan, arm, repetition)`. Comparison is **paired per plan**:
each plan is its own control, and two runs are comparable iff their
`config_hash` is identical except for `arm`.

- `run_uid` — stable external id (ULID/uuid), minted by the harness.
  Raw transcripts/briefs/reviewer reports are archived in a tree keyed by
  `run_uid`, decoupled from the autoincrement `id`, surviving a DB rebuild.
- `config_hash` / `config_json` — hash is the GROUP BY key; json is the
  opaque audit blob it is taken over (model versions, tier routing,
  reviewer cadence, budgets, brief-template version). Same opaque-text
  philosophy as `agent_actions.metadata` (migration 16).
- `base_sha` — the clean-slate reset point. Every arm for a corpus member
  starts from this exact SHA in a fresh worktree.
- `arm` / `status` / `corpus_repo` — TEXT, enum enforced at the CLI parse
  layer, not the schema, so pilot/probe runs need no migration.

### `run_events` — the run's journal

Append-only, `seq`-ordered, `kind`+`payload` opaque (JSON-validated at the
CLI, text at the schema). Standalone from `agent_activity` so operational
changes or the Lua excision cannot corrupt a recorded measurement. This is
where token samples, reviewer decisions, conflict events, and budget marks
land — the substrate the deterministic metric queries read.

### `run_touches` — the declared-vs-actual harvest (RQ1)

The go/no-go instrument. Each row is one `(task, path)` touch tagged by
`kind`:

- `declared` — the predicted closure, **snapshotted into the run at start**,
  not a live FK to `task_touch_paths`. The snapshot makes the run
  self-contained and immutable: re-declaring touches, or improving the
  closure extractor, cannot retroactively rewrite a recorded prediction.
- `actual` — ground truth, harvested at fan-in from `git diff --name-only`
  against the cycle branch.

Touch-prediction **precision/recall is a self-join filtered by `kind`**:

```sql
-- per-run, per-task prediction accuracy
with d as (select task_id, path from run_touches
           where run_id = :run and kind = 'declared'),
     a as (select task_id, path from run_touches
           where run_id = :run and kind = 'actual'),
     hit as (select d.task_id, count(*) n from d
             join a using (task_id, path) group by d.task_id),
     dc as (select task_id, count(*) n from d group by task_id),
     ac as (select task_id, count(*) n from a group by task_id)
select dc.task_id,
       coalesce(hit.n,0) * 1.0 / dc.n          as precision,
       coalesce(hit.n,0) * 1.0 / ac.n          as recall
from dc join ac using (task_id) left join hit using (task_id);
```

`task_id` is a plain integer, not an FK-cascade: a run is an immutable
historical record, so deleting a task later must not delete the evidence
of what it once touched. The `run_id` cascade is the only intended
deletion path.

---

## 3. What is deliberately absent: no `run_metrics` table

Primary metrics — wall-clock, total tokens, touch precision/recall,
merge-conflict count, parallelism recovered — were computed by **named,
checked-in SQL queries** over the raw tables, living in a `metrics/`
directory, never materialized into a table.

> **Retired (plan 1065 M4, task 6865):** `scripts/bench-matrix.sh` (the
> closure-measurement confirmatory-run driver these queries read) and the
> `metrics/` query directory have been removed in favor of the M3 cost
> capture in `evals/orchestrator/`. This section is kept as the historical
> rationale for the design; it no longer describes files present in the
> tree.

Rationale: storing pre-aggregated metrics invites two failure modes the
experiment exists to avoid. Staleness (the stored number drifts from the
raw events it summarizes), and the garden-of-forking-paths (an analyst
pre-computes forty plausible metrics, three favor the thesis by chance,
and those three migrate into the abstract). Keeping the raw event stream
as the single source of truth, with metrics as inspectable queries, makes
every reported number reproducible by anyone holding the database and
keeps the pre-registration honest: the primary-metric queries are frozen
and committed *before* the runs begin.

The analyst agents (failure taxonomy, anomaly surfacing) read these same
raw tables; they never write metrics back. Their qualitative output is
disclosed and human-spot-checked, separate from the deterministic
primary-metric path.

---

## 4. Build order

1. `00020_runs` migration (this) + the `planar run *` verb group in the
   engine. Small; unblocks everything; survives the excision by construction.
2. The `git diff` harvest (`planar run harvest`) — path-level first
   (cheap, ships now). Symbol-level harvest is a later refinement gated on
   whether path-level precision is already adequate (see granularity note).
3. Pilot: one plan, one repetition, `strict` + `eligibility` arms only
   (grouping does not exist yet). Validates the pipeline end-to-end and
   produces the first RQ1 number — the program's go/no-go.
4. Only then: the grouping verb, and the full corpus × 3 arms × N runs.

The instrument is built and proven against the existing two arms *before*
the headline feature exists. By the time grouping lands, it aims at a rig
that already works.
