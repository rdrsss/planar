

-- ============================================================
-- annotations: line-anchored notes on code
-- ============================================================

-- annotations: a single note anchored to a file and optional line range,
-- scoped via (scope_kind, scope_id), carrying body, optional title/slug,
-- a status, and a stable anchor descriptor (path, line range, commit SHA,
-- text hash). The anchor descriptor is enough to compute anchor state
-- (fresh / drifted / stale) against any commit; state itself is not
-- stored.
create table annotations (
  id                integer primary key autoincrement,
  scope_kind        text    not null check (scope_kind in ('global','repo','association')),
  scope_id          integer,
  anchor_path       text    not null,
  anchor_line_start integer,
  anchor_line_end   integer,
  anchor_commit_sha text    not null default '',
  anchor_text_hash  text    not null default '',
  anchor_text       text    not null default '',
  title             text,
  slug              text,
  body              text    not null default '',
  status            text    not null default 'active'
                                  check (status in ('active','resolved','dismissed','archived')),
  vendor            text    not null default '',
  plan_id           integer references plans(id) on delete set null,
  task_id           integer references tasks(id) on delete set null,
  created_at        text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at        text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);

create index ix_annotations_scope        on annotations(scope_kind, scope_id);
create index ix_annotations_anchor_path  on annotations(anchor_path);
create index ix_annotations_plan_id      on annotations(plan_id) where plan_id is not null;
create index ix_annotations_task_id      on annotations(task_id) where task_id is not null;
create index ix_annotations_status       on annotations(status);
create index ix_annotations_created_at   on annotations(created_at);
create unique index ux_annotations_slug  on annotations(slug) where slug is not null;

-- ============================================================
-- annotation_tags: many-to-many tags on annotations
-- ============================================================

-- annotation_tags: simple junction holding one row per (annotation, tag)
-- pair. Cascade-deletes when the parent annotation is removed. The
-- composite primary key dedupes duplicate (annotation_id, tag) pairs at
-- the schema level.
create table annotation_tags (
  annotation_id integer not null references annotations(id) on delete cascade,
  tag           text    not null,
  primary key (annotation_id, tag)
);

create index ix_annotation_tags_tag on annotation_tags(tag);

-- ============================================================
-- search_annotations: FTS5 index over annotations.(title, body)
-- ============================================================
--
-- Self-contained FTS5 (no external content) to match the pattern set by
-- 0011 for the other six entity kinds. Tokenizer is unicode61 — same as
-- 0011 — for case folding and diacritic stripping. Title is nullable on
-- annotations, so the trigger bodies coalesce it to '' before insert.

create virtual table search_annotations using fts5(
  title, body,
  tokenize='unicode61'
);

-- Each trigger body contains multiple statements; wrap in goose
-- StatementBegin/End so the goose `;` splitter does not chop trigger
-- bodies into invalid fragments. Same pattern as 0011.

create trigger annotations_search_insert after insert on annotations begin
  insert into search_annotations(rowid, title, body)
    values (new.id, coalesce(new.title, ''), new.body);
end;

create trigger annotations_search_delete after delete on annotations begin
  delete from search_annotations where rowid = old.id;
end;

create trigger annotations_search_update after update on annotations begin
  delete from search_annotations where rowid = old.id;
  insert into search_annotations(rowid, title, body)
    values (new.id, coalesce(new.title, ''), new.body);
end;

-- ============================================================
-- Rebuild entity_links with 'annotation' in the from_kind / to_kind CHECK
-- ============================================================
--
-- SQLite has no ALTER TABLE ... DROP CONSTRAINT. Canonical pattern: create
-- a new table with the extended CHECK, copy rows, drop old, rename new,
-- re-create indexes. Mirrors the technique used in 0008.

create table entity_links_new (
  id           integer primary key autoincrement,
  from_kind    text not null check(from_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision','session','repo','annotation'
  )),
  from_id      integer not null,
  to_kind      text not null check(to_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision','session','repo','annotation'
  )),
  to_id        integer not null,
  relationship text not null check(relationship in (
    'derives-from','blocks','addresses','verifies','cites','supersedes','touches'
  )),
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (from_kind, from_id, to_kind, to_id, relationship)
);

insert into entity_links_new
  select id, from_kind, from_id, to_kind, to_id, relationship, created_at
  from entity_links;

drop table entity_links;

alter table entity_links_new rename to entity_links;

create index ix_entity_links_from on entity_links(from_kind, from_id);
create index ix_entity_links_to on entity_links(to_kind, to_id);
create index ix_entity_links_relationship on entity_links(relationship);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (12, 'annotations: line-anchored notes with FTS5 indexing and entity_links widening');
