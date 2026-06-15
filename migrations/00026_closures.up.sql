-- ============================================================
-- closures: the derived-closure snapshot (M2, the contribution)
-- ============================================================
-- One row per (task, symbol unit) in a task's *derived* closure — the
-- symbols the task must hold resident, COMPUTED from its declared seed
-- paths by static analysis (engine/closure/), not asserted via
-- task_touch_paths. This is the experimental object the decomposition
-- paper measures against the declared-touch baseline (see
-- docs/research/closure-measurement-build-spec.md §3).
--
-- Both `path` and `symbol` are stored. The qualified-name scheme is
-- stem-only (<file-stem>.<decl>), so without the repo-relative `path`
-- two same-stem files in different directories collide once their
-- modify-sets coexist in this table (the M2.2 reviewer caveat). Keeping
-- `path` lets a consumer disambiguate and lets a future milestone
-- re-resolve a symbol back to its defining file.
--
-- role partitions the closure (spec v0.1 §1.2): 'modify' = the seed's
-- own edited symbols; 'reference' = the interfaces it depends on;
-- 'transitive' = deeper hops. Transitive rows ARE stored (so the
-- extractor's decisions stay auditable and promotion experiments need
-- no re-extraction) but are EXCLUDED from the effective closure by
-- default. token_weight is the unit's raw Zig-token count (M2.4);
-- extractor_version records which extractor produced the row so a
-- re-extraction with a newer algorithm is comparable, not silently
-- overwritten. task_id / repo_id cascade so deleting a task or repo
-- reaps its derived closure.
create table closures (
  id                integer primary key autoincrement,
  task_id           integer not null references tasks(id) on delete cascade,
  repo_id           integer not null references projects(id) on delete cascade,
  path              text    not null,
  symbol            text    not null,
  role              text    not null check (role in ('modify', 'reference', 'transitive')),
  token_weight      integer not null default 0,
  extractor_version text    not null,
  created_at        text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  unique (task_id, repo_id, path, symbol, role, extractor_version)
);

create index ix_closures_task on closures(task_id);
create index ix_closures_repo on closures(repo_id);
create index ix_closures_task_role on closures(task_id, role);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (26, 'closures: derived symbol-level closure snapshot per task (M2 extractor)');
