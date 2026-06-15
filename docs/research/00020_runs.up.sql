-- ============================================================
-- runs: the benchmark instrument for the decomposition experiment
-- ============================================================
--
-- This migration adds the measurement substrate for the vertical-slice
-- experiment (strict / eligibility / grouped arms over a frozen corpus).
-- It is DELIBERATELY decoupled from the operational tables (agent_actions,
-- agent_activity, sessions) and from the planar-execute Lua layer.
--
-- ## Why these tables live in the engine, not in planar-execute
--
-- planar-execute's state/schema/brief modules hold NO SQLite handle by
-- design (see their capability-boundary docblocks). They read the DB only
-- by shelling `planar … --json`. The benchmark must WRITE durable records,
-- so the run-record subsystem belongs to the `planar` engine + a `planar
-- run *` CLI verb group, reached by the harness via subprocess — the same
-- pattern the harness already uses for reads.
--
-- Consequence (the protected-instrument invariant): the Lua execution
-- engine can be excised (spec v0.1 §5 step 1) WITHOUT touching this schema,
-- the engine code, or the `planar run *` verbs, because none of them import
-- the Lua layer. A run can be produced by a Lua workflow, by a bare
-- instrumented loop, or by a future host-native driver, and recorded
-- identically. The instrument is orthogonal to the execution mechanism.
--
-- ## What is NOT here, on purpose
--
-- No `run_metrics` table. Primary metrics (wall-clock, tokens, touch
-- precision/recall, conflict rate, parallelism recovered) are computed by
-- named, checked-in SQL queries over these raw tables — never stored.
-- Storing computed metrics invites staleness and the garden-of-forking-
-- paths (promoting whichever pre-aggregated number happened to cooperate).
-- The raw substrate is the source of truth; metrics are views.


-- ------------------------------------------------------------
-- runs: one row per (plan, arm, repetition) execution.
-- ------------------------------------------------------------
-- The unit of the experiment. `run_uid` is a stable external identifier
-- (ULID/uuid, minted by the harness) so raw transcripts, briefs, and
-- reviewer reports can be archived in a directory tree keyed by run_uid
-- independent of the autoincrement id, and so a run survives a DB rebuild.
--
-- `arm`, `status`, and `corpus_repo` are TEXT validated at the CLI parse
-- layer, not constrained at the schema layer — the same opaque-text
-- philosophy as agent_actions.metadata (migration 16): keeping them TEXT
-- lets the harness write probe/scratch values during pilot runs without a
-- schema change, while the `planar run record` verb enforces the enum.
--
-- `config_hash` is what you GROUP BY when comparing cells; `config_json`
-- is the full opaque audit blob the hash is taken over (model versions,
-- tier routing, reviewer cadence, budgets, brief-template version). Two
-- runs are comparable iff their config_hash matches except for `arm`.
--
-- `base_sha` pins the clean-slate reset point; every run of every arm for
-- a given corpus member starts from this exact SHA in a fresh worktree.
create table runs (
  id           integer primary key autoincrement,
  run_uid      text    not null,
  plan_id      integer not null references plans(id) on delete cascade,
  arm          text    not null,
  repetition   integer not null default 1,
  corpus_repo  text    not null,
  base_sha     text    not null,
  config_hash  text    not null,
  config_json  text,
  status       text    not null default 'running',
  started_at   text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  ended_at     text,
  created_at   text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (run_uid)
);

create index ix_runs_plan on runs(plan_id);
create index ix_runs_cell on runs(config_hash, arm, plan_id);


-- ------------------------------------------------------------
-- run_events: append-only raw event stream, one run's journal.
-- ------------------------------------------------------------
-- The benchmark's source-of-truth event log, deliberately standalone from
-- agent_activity (operational) so operational changes or the Lua excision
-- cannot corrupt a recorded measurement. `seq` is a monotonic per-run
-- ordinal the harness assigns (the journal analogue); `kind` and `payload`
-- are opaque (e.g. kind='token_sample' payload='{"in":1240,"out":880}',
-- kind='reviewer_decision' payload='{"outcome":"request-changes","iter":2}',
-- kind='conflict' payload='{"files":["strategy.zig"]}'). The CLI validates
-- payload is well-formed JSON; the schema treats it as text.
create table run_events (
  id           integer primary key autoincrement,
  run_id       integer not null references runs(id) on delete cascade,
  seq          integer not null,
  kind         text    not null,
  payload      text,
  occurred_at  text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  created_at   text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (run_id, seq)
);

create index ix_run_events_run on run_events(run_id);
create index ix_run_events_kind on run_events(run_id, kind);


-- ------------------------------------------------------------
-- run_touches: the declared-vs-actual harvest (RQ1 instrument).
-- ------------------------------------------------------------
-- Each row is one (task, path) touch, tagged by `kind`:
--   'declared' — the a-priori predicted closure, SNAPSHOTTED into the run
--                at start (NOT a live FK to task_touch_paths). The snapshot
--                makes the run self-contained and immutable: re-declaring
--                touches between runs, or evolving the closure extractor,
--                cannot retroactively alter a recorded run's prediction.
--   'actual'   — the a-posteriori ground truth, harvested at fan-in from
--                `git diff --name-only` against the cycle branch.
--
-- Touch-prediction precision/recall is then a self-join filtered by kind
-- (declared ∩ actual over declared = precision; over actual = recall),
-- per run and per task. This is RQ1 — and the go/no-go gate for the whole
-- program — falling out of one table.
--
-- task_id is stored as a plain integer (not an FK-cascade) because a run
-- is an immutable historical record: deleting a task later must NOT silently
-- delete the evidence of what it once touched. The run_id FK cascade is the
-- intended deletion path (drop a run, drop its touches); task lifecycle is
-- decoupled.
create table run_touches (
  id          integer primary key autoincrement,
  run_id      integer not null references runs(id) on delete cascade,
  task_id     integer not null,
  path        text    not null,
  kind        text    not null,
  created_at  text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (run_id, task_id, path, kind)
);

create index ix_run_touches_run on run_touches(run_id);
create index ix_run_touches_join on run_touches(run_id, task_id, kind);


-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (20, 'runs: benchmark instrument — run identity, event stream, declared-vs-actual touch harvest');
