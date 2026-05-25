-- Rolls back 00008_doc_artifact_kinds.up.sql


-- ============================================================
-- Rebuild artifacts with the original (narrower) kind CHECK
-- ============================================================

-- artifacts_old: scratch table mirroring artifacts shape but with the original
-- (pre-0008) kind CHECK. The INSERT will fail if any artifact row currently
-- holds one of the four new kinds — that is the correct behavior, because
-- rolling back the constraint while live data depends on it would corrupt the
-- schema contract.
create table artifacts_old (
  id          integer primary key autoincrement,
  scope_kind  text not null check(scope_kind in ('repo','association','global')),
  scope_id    integer,
  kind        text not null check(kind in (
    'tech_spec','adr','design_note','summary','readme','generated','other',
    'product_spec','roadmap'
  )),
  title       text not null,
  body        text,
  source_path text,
  status      text not null default 'active' check(status in ('draft','active','superseded','retired')),
  created_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);

insert into artifacts_old
  select id, scope_kind, scope_id, kind, title, body, source_path, status, created_at, updated_at
  from artifacts;

drop table artifacts;

alter table artifacts_old rename to artifacts;

create index ix_artifacts_scope on artifacts(scope_kind, scope_id);
create index ix_artifacts_kind on artifacts(kind);
create index ix_artifacts_status on artifacts(status);
create index ix_artifacts_source_path on artifacts(source_path) where source_path is not null;

delete from schema_migrations where version = 8;
