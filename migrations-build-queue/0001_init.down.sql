-- Rolls back 0001_init.up.sql.
--
-- Nothing guards this rollback: rollback_all does not wrap a down script in a
-- transaction, and no row here is held by a live process. Callers drain the
-- queue before rolling back a database that has one. Dropping queue_entries
-- also removes its sqlite_sequence row, so a later re-apply starts again at
-- the 2000000 floor.

drop table queue_settings;

drop table queue_identity;

drop table queue_schema;

drop table queue_history;

drop table queue_entries;

-- The version row goes first, then the table that holds it. The foundation
-- migration's down drops that table (migrations/README.md), so the chain is
-- empty again afterwards.
delete from schema_migrations where version = 1;

drop table schema_migrations;
