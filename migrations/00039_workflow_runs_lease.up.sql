-- 00039 workflow_runs_lease — plan 1065 M2 (decision D11, task 6846):
-- `planar-agent run start` gains a pid-less, TTL-supervised shape for runs
-- that are not tied to a live OS process the caller can pid-probe. D11
-- verbatim: "A run without a pid must carry `expires_at`, extended by `run
-- heartbeat`, and is abandoned by `reconcile` on expiry. Rationale: it is
-- the model the claim ledger already uses; 'externally supervised, never
-- abandoned' leaves zombie rows, and deriving liveness from claims is
-- subtle."
--
-- Two changes to `workflow_runs`, and nothing else:
--
--   pid           integer not null  ->  integer, nullable
--   expires_at    (new) text, nullable — the pid-less run's lease deadline
--   CHECK (pid is not null or expires_at is not null) — a run must be
--     supervised one way or the other
--
-- Every pre-existing row keeps its meaning: it has a pid and no
-- `expires_at`, which the CHECK admits unchanged.
--
-- ## Why this is a `writable_schema` text edit, not a table rebuild
--
-- Migration 00038 measured (see its own header) that rebuilding
-- `workflow_runs` — create-new / copy / drop-old in any order — fires
-- `DROP TABLE`'s implicit DELETE against the OLD table, which cascades
-- through `agent_work_claims.run_id` (ON DELETE SET NULL) and
-- `context_records.run_id` (ON DELETE CASCADE) because this migration
-- runs inside a transaction with `foreign_keys` ON, which cannot be
-- switched off mid-transaction. `workflow_runs` is a foreign-key parent of
-- both tables, so a rebuild here would repeat exactly the corruption 00038
-- was written to avoid. This migration follows 00038's own precedent:
-- `pid`'s NOT NULL is relaxed, and the CHECK is added, by editing the
-- stored `CREATE TABLE` text under `writable_schema`, after `expires_at`
-- is added the ordinary way (`ALTER TABLE ... ADD COLUMN`, which must run
-- BEFORE the text edits — it splices into the stored text at an offset
-- remembered from the connection's last parse). No table is dropped, so
-- no foreign-key action can fire and no row moves. The guard after the
-- edits fails the migration by name if either replacement did not take,
-- rather than recording version 39 over an unrelaxed constraint.

alter table workflow_runs add column expires_at text;

pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  'pid           integer not null,',
  'pid           integer,'
)
where type = 'table' and name = 'workflow_runs';

update sqlite_schema
set sql = replace(
  sql,
  ', expires_at text)',
  ', expires_at text, check (pid is not null or expires_at is not null))'
)
where type = 'table' and name = 'workflow_runs';

-- Clears the flag AND makes this connection re-read sqlite_schema, so the
-- relaxed pid constraint and the new CHECK hold for every statement after
-- this one, including a later migration in the same `apply_all` pass.
pragma writable_schema = reset;

-- workflow_runs: a run's lease. `expires_at` is the pid-less lease deadline
-- (D11); the partial index only ever needs to find running, pid-less runs.
create index ix_workflow_runs_expires on workflow_runs(expires_at) where status = 'running';

-- Fail by name if either edit did not land. TEMP, so it never touches the
-- database file; dropped on success, rolled back with everything else on
-- failure.
create temp table m00039_up_guard (
  refusal text
    constraint m00039_up_refused_pid_not_relaxed_or_lease_check_missing
    check (refusal is null)
);
insert into temp.m00039_up_guard (refusal)
select 'unrelaxed'
where not exists (
  select 1 from sqlite_schema
  where type = 'table' and name = 'workflow_runs' and sql like '%pid           integer,%'
)
or not exists (
  select 1 from sqlite_schema
  where type = 'table' and name = 'workflow_runs'
    and sql like '%check (pid is not null or expires_at is not null)%'
);
drop table temp.m00039_up_guard;

insert into schema_migrations (version, description)
values (39, 'workflow_runs: nullable pid, lease expires_at, CHECK one of pid/expires_at is set (decision D11)');
