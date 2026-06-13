-- context_records: make claim_id nullable to support run-keyed capsule writes.
--
-- Decision 456 (plan 585 stage-close compaction): when the orchestrator writes
-- a compiled capsule via `planar-agent context capsule --run <id> ...`, the
-- write is keyed to the workflow_runs row, NOT to a worker claim. The original
-- schema (migration 22) declared `claim_id NOT NULL`; this migration relaxes
-- that constraint so capsule rows can carry claim_id = NULL.
--
-- SQLite does not support ALTER COLUMN to change nullability, so we use the
-- standard table-rebuild pattern (rename → recreate → copy → drop). Indexes
-- are also rebuilt to match the original migration-22 shape.
--
-- The rebuild changes NOTHING except claim_id nullability; all other columns,
-- types, constraints, and FK references are preserved exactly.

-- 1. Rename the existing table.
alter table context_records rename to context_records_old;

-- 2. Recreate with claim_id nullable.
create table context_records (
  id            integer primary key autoincrement,
  run_id        integer not null references workflow_runs(id) on delete cascade,
  stage         text not null,
  session_id    integer not null references sessions(id) on delete restrict,
  claim_id      integer references agent_work_claims(id) on delete restrict,
  kind          text not null check(kind in (
    'finding','risk','artifact','followup','summary','capsule'
  )),
  body          text not null,
  status        text not null default 'active' check(status in (
    'active','consumed','superseded'
  )),
  compiled_from text,
  created_at    text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

-- 3. Copy existing data (claim_id values preserved; NULLs silently pass now).
insert into context_records (id, run_id, stage, session_id, claim_id, kind, body, status, compiled_from, created_at)
select id, run_id, stage, session_id, claim_id, kind, body, status, compiled_from, created_at
from context_records_old;

-- 4. Drop the old table.
drop table context_records_old;

-- 5. Recreate indexes (same as migration 22).
create index ix_context_records_run on context_records(run_id);
create index ix_context_records_stage on context_records(run_id, stage);
create index ix_context_records_claim on context_records(claim_id);
create index ix_context_records_session on context_records(session_id);
create index ix_context_records_active on context_records(run_id, stage, status) where status = 'active';
create index ix_context_records_kind on context_records(kind);

insert into schema_migrations (version, description)
values (24, 'context_records: make claim_id nullable for run-keyed capsule writes (decision 456)');
