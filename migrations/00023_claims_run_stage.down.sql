-- Revert 00023: drop run_id index then drop the two columns.
-- SQLite 3.35+ supports ALTER TABLE ... DROP COLUMN; the vendored
-- amalgamation in vendor/sqlite/ clears that bar. The index must be
-- dropped first because it references the column.
drop index if exists ix_agent_work_claims_run;

alter table agent_work_claims drop column stage;
alter table agent_work_claims drop column run_id;

delete from schema_migrations where version = 23;
