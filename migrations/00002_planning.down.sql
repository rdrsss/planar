-- Rolls back 00002_planning.up.sql


drop table if exists decisions;
drop table if exists artifacts;
drop table if exists plans;

delete from schema_migrations where version = 2;
