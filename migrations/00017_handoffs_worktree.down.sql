-- SQLite 3.35+ supports `alter table ... drop column`. The vendored
-- amalgamation under `vendor/sqlite/` is well past that bar.
alter table handoffs drop column branch;
alter table handoffs drop column repo_root;
alter table handoffs drop column worktree_path;

delete from schema_migrations where version = 17;
