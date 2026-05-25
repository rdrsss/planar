

-- ============================================================
-- Rebuild artifacts with the extended kind CHECK (adds test_spec)
-- ============================================================

-- artifacts_new: scratch table mirroring artifacts shape but with the
-- extended kind CHECK constraint. Populated from the existing artifacts
-- table, then renamed into place. Exists only for the duration of this
-- migration.
create table artifacts_new (
  id          integer primary key autoincrement,
  scope_kind  text not null check(scope_kind in ('repo','association','global')),
  scope_id    integer,
  kind        text not null check(kind in (
    'tech_spec','adr','design_note','summary','readme','generated','other',
    'product_spec','roadmap',
    'research','getting_started','changelog_entry','glossary_term',
    'test_spec'
  )),
  title       text not null,
  body        text,
  source_path text,
  slug        text,
  status      text not null default 'active' check(status in ('draft','active','superseded','retired')),
  created_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);

insert into artifacts_new
  select id, scope_kind, scope_id, kind, title, body, source_path, slug, status, created_at, updated_at
  from artifacts;

drop table artifacts;

alter table artifacts_new rename to artifacts;

create index ix_artifacts_scope on artifacts(scope_kind, scope_id);
create index ix_artifacts_kind on artifacts(kind);
create index ix_artifacts_status on artifacts(status);
create index ix_artifacts_source_path on artifacts(source_path) where source_path is not null;
create unique index ux_artifacts_slug on artifacts(slug) where slug is not null;

-- ============================================================
-- Re-create the artifacts_fts triggers from 0011
-- ============================================================
--
-- SQLite drops triggers when their source table is dropped. The
-- table-rebuild above drops the original artifacts table, taking the
-- artifacts_fts_ai / artifacts_fts_au / artifacts_fts_ad triggers
-- (originally created by 0011) with it. Recreate them so future writes
-- to artifacts continue to propagate into the FTS index.

create trigger artifacts_fts_ai after insert on artifacts begin
  insert into artifacts_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;
create trigger artifacts_fts_ad after delete on artifacts begin
  delete from artifacts_fts where rowid = old.id;
end;
create trigger artifacts_fts_au after update on artifacts begin
  delete from artifacts_fts where rowid = old.id;
  insert into artifacts_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (13, 'artifacts.kind: add test_spec');
