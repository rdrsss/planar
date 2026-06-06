-- ============================================================
-- task_touch_paths: path-level touch declarations for a task
-- ============================================================

-- task_touch_paths: the repo-relative file paths a task is expected to
-- modify. Path-level granularity for the parallelizability rules (plan 492
-- M5, decision 370): rule 2 (disjoint touch sets), rule 3 (migration
-- touched), and rule 4 (singleton authoritative file touched) all need
-- file-path precision. The coarse repo-level signal already exists as an
-- entity_links(from_kind='task', to_kind='repo', relationship='touches')
-- edge; this table is ADDITIVE and coexists with it — a path-touch implies
-- the repo-touch (the CLI writes both) so the repo-level signal stays
-- consistent, while this table adds the file-path detail the rules consume.
-- repo_id references projects(id) (a "repo" is a row in the projects table,
-- matching how the entity_links touches edge resolves its to_id via the
-- repo slug). Both FKs cascade-delete so removing a task or repo cleans up
-- its declared path touches.
create table task_touch_paths (
  id         integer primary key autoincrement,
  task_id    integer not null references tasks(id) on delete cascade,
  repo_id    integer not null references projects(id) on delete cascade,
  path       text    not null,
  created_at text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (task_id, repo_id, path)
);

create index ix_task_touch_paths_task on task_touch_paths(task_id);
create index ix_task_touch_paths_repo on task_touch_paths(repo_id);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (19, 'task_touch_paths: path-level task-touch declarations for the parallelizability rules');
