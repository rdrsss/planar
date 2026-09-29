-- ============================================================
-- Agent database foundation (plan 1080, decision 1181)
-- ============================================================

-- agent_schema_migrations: the agent database's schema-version contract,
-- independent of the main database's schema_migrations. Every agent
-- migration inserts one row. The row with the highest version is
-- authoritative: its compat column names the oldest binary schema version
-- that may open this store, so a migration that only adds tables, columns
-- with defaults, or indexes keeps the previous compat, and one that drops,
-- renames or changes the meaning of anything sets compat to its own version.
create table agent_schema_migrations (
  version     integer primary key,
  compat      integer not null,
  description text not null
);

-- ============================================================
-- Record the migration
-- ============================================================

insert into agent_schema_migrations (version, compat, description)
values (1, 1, 'agent database foundation');
