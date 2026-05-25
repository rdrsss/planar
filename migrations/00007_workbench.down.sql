-- Rolls back 00007_workbench.up.sql


drop table if exists workbench_sync_state;

delete from schema_migrations where version = 7;
