drop table session_commits;

-- SQLite 3.35+ supports `alter table ... drop column`. The vendored
-- amalgamation under `vendor/sqlite/` is well past that bar.
alter table sessions drop column head_sha_at_start;
alter table sessions drop column repo_root;

delete from schema_migrations where version = 21;
