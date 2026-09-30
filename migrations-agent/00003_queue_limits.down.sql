-- Rolls back 00003_queue_limits.up.sql
--
-- SQLite (the vendored amalgamation is 3.53) drops a plain column in place;
-- none of these columns is indexed, constrained or referenced, so no table
-- rebuild is needed and every other column and row is kept.

alter table queue_history drop column wait_limit_ms;
alter table queue_history drop column run_limit_ms;

alter table queue_entries drop column wait_limit_ms;
alter table queue_entries drop column run_limit_ms;

delete from agent_schema_migrations where version = 3;
