-- Rolls back 00005_sessions.up.sql


drop table if exists handoffs;
drop table if exists context_snapshots;
drop table if exists session_entries;
drop table if exists sessions;

delete from schema_migrations where version = 5;
