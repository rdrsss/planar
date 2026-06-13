-- agent_work_claims run/stage correlation: two nullable columns that let
-- planar-execute associate a claim with the workflow_runs row it was
-- acquired under and the workflow stage that dispatched the worker.
--
-- run_id references workflow_runs(id): populated by planar-agent pull /
-- claim when the caller passes --run <id>. NULL for interactive operator
-- claims that are not part of a workflow run.
--
-- stage is the free-text stage name the worker is executing (e.g.
-- "plan", "code", "review"). NULL when not inside a run, or when the
-- caller omits --stage even with --run.
--
-- SQLite's ALTER TABLE ADD COLUMN supports a REFERENCES clause only when
-- the column has DEFAULT NULL (SQLite 3.37+). The vendored amalgamation
-- in vendor/sqlite/ clears that bar. The FK is advisory — PRAGMA
-- foreign_keys=ON is set per-connection in the engine, so the referential
-- guard is active at runtime even though it is not enforced in the DDL
-- at column-add time the same way a full table rebuild would express it.
alter table agent_work_claims add column run_id integer references workflow_runs(id) on delete set null;
alter table agent_work_claims add column stage text;

create index ix_agent_work_claims_run on agent_work_claims(run_id) where run_id is not null;

insert into schema_migrations (version, description)
values (23, 'agent_work_claims: nullable run_id (FK workflow_runs) + stage for planar-execute correlation');
