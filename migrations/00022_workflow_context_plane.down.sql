drop table context_records;
drop table workflow_runs;

delete from schema_migrations where version = 22;
