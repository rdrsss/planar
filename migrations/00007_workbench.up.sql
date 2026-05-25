

-- ============================================================
-- Workbench sync state
-- ============================================================

-- workbench_sync_state: tracks the relationship between a file in the
-- workbench filesystem and the entity it represents in the database.
-- Exists so workbench pull/push/status can detect FS-only, DB-only, and
-- conflicting changes without re-reading every file on every run.
create table workbench_sync_state (
  id             integer primary key,
  anchor_plan_id integer not null references plans(id) on delete cascade,
  entity_kind    text not null check(entity_kind in (
    'plan', 'task', 'artifact', 'scenario', 'decision', 'question'
  )),
  entity_id      integer not null,
  file_path      text not null,            -- relative to PLANAR_WORKBENCH_ROOT
  content_hash   text not null,            -- sha256 of the file body at last sync
  fs_mtime       text,                     -- ISO 8601 mtime of the file at last sync, nullable for never-synced files
  db_updated_at  text not null,            -- entity.updated_at value at last sync
  last_synced_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique(file_path),
  unique(entity_kind, entity_id)
);

create index ix_wbsync_anchor on workbench_sync_state(anchor_plan_id);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (7, 'sync_events: extend outcome CHECK for strategy-abandoned + counterpart-missing');
