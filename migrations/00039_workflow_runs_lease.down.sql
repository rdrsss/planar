-- 00039 workflow_runs_lease, reversed. See the up file for why the pid
-- relaxation and the lease CHECK are `writable_schema` text edits rather
-- than a table rebuild.
--
-- ## Pid-less rows are deleted, not refused
--
-- Unlike 00038's down (which refuses by name rather than discard rows the
-- older schema cannot represent), this rollback deletes any row with a
-- NULL `pid` before restoring `pid NOT NULL` — a pid-less run is, by
-- construction, a row the pre-00039 schema never could have held, and
-- there is no caller-facing state (no claim references it that the older
-- schema depended on) that a refusal would protect. This matches the
-- task's own down contract. `agent_work_claims.run_id` is
-- `ON DELETE SET NULL`, so a claim that happened to reference a deleted
-- pid-less run is left with a NULL `run_id` rather than being destroyed.
-- `context_records.run_id` is `ON DELETE CASCADE`, though, so this DELETE
-- also destroys every context record belonging to a pid-less run. That is
-- accepted for the same reason — those records exist only for runs the
-- pre-00039 schema could not hold — but it IS data loss on rollback and is
-- named here rather than left to be discovered from the schema.

delete from workflow_runs where pid is null;

drop index ix_workflow_runs_expires;

-- Strip the CHECK first — DROP COLUMN refuses on a column a CHECK still
-- names — then drop the now-unreferenced column, then restore pid's NOT
-- NULL. Order matters the same way the up file's ADD COLUMN-before-edits
-- ordering does: each ALTER TABLE re-splices the stored text at an offset
-- remembered from the connection's last parse, so it must not follow a
-- writable_schema edit it has not seen.
pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  ', check (pid is not null or expires_at is not null))',
  ')'
)
where type = 'table' and name = 'workflow_runs';

pragma writable_schema = reset;

alter table workflow_runs drop column expires_at;

pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  'pid           integer,',
  'pid           integer not null,'
)
where type = 'table' and name = 'workflow_runs';

pragma writable_schema = reset;

delete from schema_migrations where version = 39;
