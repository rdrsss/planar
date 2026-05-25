-- Rolls back 00010_task_reopens.up.sql


drop table if exists task_reopens;

delete from schema_migrations where version = 10;
