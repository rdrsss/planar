-- Rolls back 00002_queue_tables.up.sql

drop index if exists idx_queue_history_ended_at;
drop table if exists queue_history;

drop index if exists idx_queue_entries_parent_seq;
drop index if exists idx_queue_entries_state;
drop table if exists queue_entries;

delete from agent_schema_migrations where version = 2;
