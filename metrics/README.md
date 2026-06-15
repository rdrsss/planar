# metrics/ — Primary Metric Queries

These SQL queries implement the frozen metric definitions from
`docs/research/preregistration.md §2`. They run against a `planar` SQLite
database via the `sqlite3` CLI; they are **not** embedded in the binary.

Per preregistration §3 (§7.1 query→RQ mapping): no metric is stored in a
table. Every reported number is reproducible from the raw tables by running
the named query here.

---

## rq1_touch_accuracy.sql — M-PREC / M-REC

**RQ:** RQ1 (the go/no-go gate)
**Metrics:** M-PREC (touch prediction precision), M-REC (touch prediction recall)
**Source tables:** `run_touches` (self-join on `kind`)
**§7.1 mapping:** `rq1_touch_accuracy.sql` → `run_touches (self-join on kind)`

Computes path-level precision and recall of declared touch closure against
the actual `git diff --name-only` ground truth, per `(run, task)`.

Frozen definitions (preregistration §2):

- precision = |declared ∩ actual| / |declared|
- recall    = |declared ∩ actual| / |actual|
- `declared` = `run_touches` rows with `kind='declared'` (snapshot at run start)
- `actual`   = `run_touches` rows with `kind='actual'` (harvested at fan-in)

Edge cases (frozen — preregistration §2):

- |declared| = 0: precision is **undefined** — the task is **excluded** from
  the precision average and counted separately as `undeclared_count`.
- |actual| = 0: recall is **undefined** — the task is **excluded** from the
  recall average and counted separately as `no_actual_count`.
- Aggregate = **macro-average** across included tasks (not micro/weighted).
- The full per-task distribution is reported (Query 1), not only the mean.

The file contains **two queries**:

1. **Per-task distribution** — `(task_id, declared_count, actual_count,
   hit_count, precision, recall)`. precision/recall are NULL for excluded
   tasks. This is the full distribution preregistration §2 requires.
2. **Aggregate summary** — `(macro_avg_precision, macro_avg_recall,
   undeclared_count, no_actual_count, precision_task_count,
   recall_task_count, total_task_count)`. The macro averages exclude
   NULL-precision / NULL-recall tasks respectively (SQLite `avg()` ignores NULLs).

**Run:**
```sh
sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/rq1_touch_accuracy.sql
```

---

## tokens_per_plan.sql — M-TOK

**RQ:** RQ2
**Metric:** M-TOK (total tokens per plan, all agents including reviewers)
**Source tables:** `runs`, `run_events` (kind='token_sample')
**§7.1 mapping:** `tokens_per_plan.sql` → `runs, run_events`

Sums input + output tokens across ALL agents in a run, including reviewer
agents (preregistration §2: "reviewer cost is part of an arm's cost").
Primary figure is raw billed tokens; cache-adjusted is a secondary column,
reported alongside, never substituted (preregistration §2).

Payload schema: `{"in": <int>, "out": <int>}` (raw);
optionally `{"in": <int>, "out": <int>, "cache_in": <int>, "cache_out": <int>}`.

**NOTE:** `token_sample` events are emitted by the harness during agent
dispatch. This query returns 0/empty against current data — the shape is
correct; results appear once the harness runs.

**Run:**
```sh
sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/tokens_per_plan.sql
```

---

## wallclock_per_plan.sql — M-WALL

**RQ:** RQ2
**Metric:** M-WALL (wall-clock seconds per plan)
**Source tables:** `runs`, `run_events` (kinds: `slice_dispatch`, `slice_fanin`)
**§7.1 mapping:** `wallclock_per_plan.sql` → `runs, run_events`

Measures seconds from the first `slice_dispatch` event to the last
`slice_fanin` event (preregistration §2: "Seconds from first slice dispatch
to last slice fan-in, per run").

`elapsed_seconds` is NULL when either event kind is absent in the run's
journal (expected while harness events are not yet emitted).

**NOTE:** These events are emitted by the harness at slice dispatch and
fan-in. Returns null elapsed against current data.

**Run:**
```sh
sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/wallclock_per_plan.sql
```

---

## conflicts.sql — M-CONF

**RQ:** RQ3
**Metric:** M-CONF (merge conflicts at fan-in)
**Source tables:** `runs`, `run_events` (kind='conflict')
**§7.1 mapping:** `conflicts.sql` → `runs, run_events`

Counts slices producing a git merge conflict against the integration branch
at fan-in (preregistration §2: "Count of slices that produce a git merge
conflict against the integration branch at fan-in, per run").

One `conflict` event per conflicting slice; payload carries the list of
conflicting files as `{"files": [...]}`. The query also sums total
conflicted files across the run as a secondary column.

**NOTE:** `conflict` events are emitted by the harness at fan-in. Returns 0
against current data.

**Run:**
```sh
sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/conflicts.sql
```

---

## Not in this M1 subset (planned for later milestones)

Per preregistration §7.1, the full query set also includes:

- `parallelism_recovered.sql` — M-PAR (RQ2), from `recommend-strategy` output
- `blast_radius.sql` — M-BLAST (RQ3), from `run_events` slice-failure events
- `reviewer_iters.sql` — M-ITER (RQ3), from `run_events` reviewer decisions
- `coupling_density.sql` — RQ4, from `closures` (M2) + `run_touches`

These are out of scope for M1.6 and will be added in the milestones that
introduce the corresponding data sources.
