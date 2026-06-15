-- ============================================================
-- runs / run_events / run_touches: the measurement-rig substrate
-- ============================================================
-- These three tables are the benchmark instrument for the
-- vertical-slice decomposition experiment (strict / eligibility /
-- grouped arms). They live in the engine behind the `planar run *` verb
-- group and are orthogonal to any execution mechanism (Lua harness, bare
-- loop, host-native driver): the same recording surface measures every
-- arm. See docs/research/run-record-schema.md §2 for the full rationale.

-- runs: one row per (plan, arm, repetition) — the experimental unit.
-- Comparison is paired per plan: each plan is its own control, and two
-- runs are comparable iff their config_hash is identical except for arm.
-- run_uid is a stable external id (ULID/uuid) minted by the harness;
-- raw transcripts/briefs/reviewer reports are archived in a tree keyed
-- by run_uid, decoupled from the autoincrement id so they survive a DB
-- rebuild. config_hash is the GROUP BY key; config_json is the opaque
-- audit blob it is taken over (model versions, tier routing, reviewer
-- cadence, budgets, brief-template version) — same opaque-text
-- philosophy as agent_actions.metadata (migration 16). base_sha is the
-- clean-slate reset point every arm starts from in a fresh worktree.
-- arm / status / corpus_repo are plain TEXT: their enum sets are
-- enforced at the CLI parse layer, not the schema, so pilot/probe runs
-- need no migration (run-record-schema.md §2).
create table runs (
  id          integer primary key autoincrement,
  run_uid     text    not null,
  plan_id     integer not null references plans(id) on delete cascade,
  arm         text    not null,
  base_sha    text    not null,
  config_hash text    not null,
  config_json text,
  corpus_repo text,
  status      text    not null default 'running',
  started_at  text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  ended_at    text,
  unique (run_uid)
);

create index ix_runs_plan on runs(plan_id);
create index ix_runs_config_hash on runs(config_hash);
create index ix_runs_arm on runs(arm);

-- run_events: the run's append-only, seq-ordered journal. kind+payload
-- are opaque (JSON-validated at the CLI, plain text at the schema). This
-- table is STANDALONE from agent_activity (decision D3) so operational
-- changes or the Lua excision cannot corrupt a recorded measurement.
-- Token samples, reviewer decisions, conflict events, and budget marks
-- land here — the substrate the deterministic primary-metric queries
-- read. The run_id FK cascades so deleting a run reaps its journal.
create table run_events (
  id         integer primary key autoincrement,
  run_id     integer not null references runs(id) on delete cascade,
  seq        integer not null,
  kind       text    not null,
  payload    text,
  created_at text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (run_id, seq)
);

create index ix_run_events_run on run_events(run_id);

-- run_touches: the declared-vs-actual touch harvest — the go/no-go
-- instrument for RQ1 (touch-prediction precision/recall). Each row is
-- one (task, path) touch tagged by kind: 'declared' is the predicted
-- closure snapshotted into the run at start (NOT a live FK to
-- task_touch_paths, so improving the closure extractor cannot
-- retroactively rewrite a recorded prediction); 'actual' is ground
-- truth harvested at fan-in from `git diff --name-only` against the
-- cycle branch. Precision/recall is a self-join filtered by kind.
-- task_id is a PLAIN INTEGER, not an FK-cascade (decision D2): a run is
-- immutable historical evidence, so deleting a task later must NOT erase
-- the record of what it once touched. The run_id cascade is the only
-- intended deletion path. kind is a closed two-value set central to
-- RQ1, so it carries a schema-level CHECK.
create table run_touches (
  id         integer primary key autoincrement,
  run_id     integer not null references runs(id) on delete cascade,
  task_id    integer not null,
  path       text    not null,
  kind       text    not null check (kind in ('declared', 'actual')),
  created_at text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (run_id, task_id, path, kind)
);

create index ix_run_touches_run on run_touches(run_id);
create index ix_run_touches_run_kind on run_touches(run_id, kind);
create index ix_run_touches_task on run_touches(task_id);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (25, 'runs/run_events/run_touches: measurement-rig substrate for the decomposition experiment');
