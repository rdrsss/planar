-- SQLite 3.35+ supports `alter table ... drop column`. The vendored
-- amalgamation under `vendor/sqlite/` is well past that bar (the
-- repo's CLAUDE.md lists the build with FTS5 + JSON1 enabled, both of
-- which post-date the drop-column release). No table-rebuild dance
-- needed.
alter table agent_actions drop column metadata;

delete from schema_migrations where version = 16;
