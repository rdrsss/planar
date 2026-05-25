-- Rolls back 00012_annotations.up.sql


-- Drop the FTS triggers explicitly so the Down direction is auditable.
drop trigger if exists annotations_search_insert;
drop trigger if exists annotations_search_update;
drop trigger if exists annotations_search_delete;

drop table if exists search_annotations;

-- ============================================================
-- Rebuild entity_links without 'annotation' in the CHECK
-- ============================================================
--
-- The INSERT will fail (correctly) if any entity_links row currently
-- references an annotation — rolling back the constraint with live data
-- referencing it would corrupt the schema contract.

create table entity_links_old (
  id           integer primary key autoincrement,
  from_kind    text not null check(from_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision','session','repo'
  )),
  from_id      integer not null,
  to_kind      text not null check(to_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision','session','repo'
  )),
  to_id        integer not null,
  relationship text not null check(relationship in (
    'derives-from','blocks','addresses','verifies','cites','supersedes','touches'
  )),
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (from_kind, from_id, to_kind, to_id, relationship)
);

insert into entity_links_old
  select id, from_kind, from_id, to_kind, to_id, relationship, created_at
  from entity_links;

drop table entity_links;

alter table entity_links_old rename to entity_links;

create index ix_entity_links_from on entity_links(from_kind, from_id);
create index ix_entity_links_to on entity_links(to_kind, to_id);
create index ix_entity_links_relationship on entity_links(relationship);

-- Drop annotation-related tables and indexes. Indexes vanish with the
-- table; the explicit DROPs make the rollback path self-documenting.

drop index if exists ix_annotation_tags_tag;
drop table if exists annotation_tags;

drop index if exists ux_annotations_slug;
drop index if exists ix_annotations_created_at;
drop index if exists ix_annotations_status;
drop index if exists ix_annotations_task_id;
drop index if exists ix_annotations_plan_id;
drop index if exists ix_annotations_anchor_path;
drop index if exists ix_annotations_scope;
drop table if exists annotations;

delete from schema_migrations where version = 12;
