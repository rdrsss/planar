-- Native entity annotations retain the legacy file-anchor columns, but make
-- the path nullable so an entity anchor is never represented by a fake file.
drop trigger if exists annotations_search_insert;
drop trigger if exists annotations_search_update;
drop trigger if exists annotations_search_delete;

create table annotations_new (
  id                integer primary key autoincrement,
  scope_kind        text    not null check (scope_kind in ('global','repo','association')),
  scope_id          integer,
  anchor_kind       text    not null default 'file' check (anchor_kind in ('file','entity')),
  anchor_path       text,
  anchor_line_start integer,
  anchor_line_end   integer,
  anchor_commit_sha text    not null default '',
  anchor_text_hash  text    not null default '',
  anchor_text       text    not null default '',
  target_kind       text    check (target_kind in ('plan','task')),
  target_id         integer,
  title             text,
  slug              text,
  body              text    not null default '',
  status            text    not null default 'active'
                                  check (status in ('active','resolved','dismissed','archived')),
  vendor            text    not null default '',
  origin            text,
  revision          integer not null default 1 check (revision >= 1),
  plan_id           integer references plans(id) on delete set null,
  task_id           integer references tasks(id) on delete set null,
  created_at        text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at        text    not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check ((scope_kind = 'global' and scope_id is null)
      or (scope_kind in ('repo','association') and scope_id is not null)),
  check ((anchor_kind = 'file' and anchor_path is not null and target_kind is null and target_id is null)
      or (anchor_kind = 'entity' and anchor_path is null and target_kind is not null and target_id is not null))
);

insert into annotations_new (
  id, scope_kind, scope_id, anchor_kind, anchor_path, anchor_line_start,
  anchor_line_end, anchor_commit_sha, anchor_text_hash, anchor_text, title,
  slug, body, status, vendor, plan_id, task_id, created_at, updated_at
)
select id, scope_kind, scope_id, 'file', anchor_path, anchor_line_start,
  anchor_line_end, anchor_commit_sha, anchor_text_hash, anchor_text, title,
  slug, body, status, vendor, plan_id, task_id, created_at, updated_at
from annotations;

-- Rebuild the dependent junction before replacing its parent. Dropping
-- annotations while annotation_tags still references it would cascade-delete
-- every legacy tag with foreign_keys enabled. Copy after the replacement
-- parent rows have been inserted so foreign-key enforcement remains active.
create table annotation_tags_new (
  annotation_id integer not null references annotations_new(id) on delete cascade,
  tag           text    not null,
  primary key (annotation_id, tag)
);
insert into annotation_tags_new select annotation_id, tag from annotation_tags;
drop table annotation_tags;

drop table annotations;
alter table annotations_new rename to annotations;
alter table annotation_tags_new rename to annotation_tags;

create index ix_annotations_scope on annotations(scope_kind, scope_id);
create index ix_annotations_anchor_path on annotations(anchor_path) where anchor_kind = 'file';
create index ix_annotations_plan_id on annotations(plan_id) where plan_id is not null;
create index ix_annotations_task_id on annotations(task_id) where task_id is not null;
create index ix_annotations_target on annotations(target_kind, target_id) where anchor_kind = 'entity';
create index ix_annotations_status on annotations(status);
create index ix_annotations_created_at on annotations(created_at);
create unique index ux_annotations_slug on annotations(slug) where slug is not null;
create index ix_annotation_tags_tag on annotation_tags(tag);

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

-- SQL constraints cannot dereference polymorphic plan/task targets. These
-- triggers make target existence, association columns, and exact scope part
-- of the durable transaction contract rather than a caller convention.
create trigger annotations_entity_target_insert before insert on annotations
when new.anchor_kind = 'entity'
begin
  select case when new.target_kind = 'plan' and
    (new.plan_id is not new.target_id or new.task_id is not null or not exists
      (select 1 from plans where id = new.target_id and scope_kind = new.scope_kind
       and (scope_id is new.scope_id))) then raise(abort, 'invalid entity annotation plan target') end;
  select case when new.target_kind = 'task' and
    (new.task_id is not new.target_id or new.plan_id is not null or not exists
      (select 1 from tasks where id = new.target_id and scope_kind = new.scope_kind
       and (scope_id is new.scope_id))) then raise(abort, 'invalid entity annotation task target') end;
end;
create trigger annotations_entity_target_update before update of anchor_kind, target_kind, target_id, plan_id, task_id, scope_kind, scope_id on annotations
when new.anchor_kind = 'entity'
begin
  select case when new.target_kind = 'plan' and
    (new.plan_id is not new.target_id or new.task_id is not null or not exists
      (select 1 from plans where id = new.target_id and scope_kind = new.scope_kind
       and (scope_id is new.scope_id))) then raise(abort, 'invalid entity annotation plan target') end;
  select case when new.target_kind = 'task' and
    (new.task_id is not new.target_id or new.plan_id is not null or not exists
      (select 1 from tasks where id = new.target_id and scope_kind = new.scope_kind
       and (scope_id is new.scope_id))) then raise(abort, 'invalid entity annotation task target') end;
end;

create trigger plans_reject_entity_annotation_delete before delete on plans
when exists (select 1 from annotations where anchor_kind = 'entity' and target_kind = 'plan' and target_id = old.id)
begin
  select raise(abort, 'cannot delete plan with entity annotations; remove annotations explicitly first');
end;
create trigger tasks_reject_entity_annotation_delete before delete on tasks
when exists (select 1 from annotations where anchor_kind = 'entity' and target_kind = 'task' and target_id = old.id)
begin
  select raise(abort, 'cannot delete task with entity annotations; remove annotations explicitly first');
end;

-- An entity annotation's scope is the target's exact scope. Refuse a target
-- scope move while it has annotations rather than leaving the durable anchor
-- mismatched. Explicit annotation removal is the disposition that permits a
-- later scope move.
create trigger plans_reject_entity_annotation_scope_update before update of scope_kind, scope_id on plans
when (new.scope_kind is not old.scope_kind or new.scope_id is not old.scope_id)
 and exists (select 1 from annotations where anchor_kind = 'entity' and target_kind = 'plan' and target_id = old.id)
begin
  select raise(abort, 'cannot change plan scope with entity annotations; remove annotations explicitly first');
end;
create trigger tasks_reject_entity_annotation_scope_update before update of scope_kind, scope_id on tasks
when (new.scope_kind is not old.scope_kind or new.scope_id is not old.scope_id)
 and exists (select 1 from annotations where anchor_kind = 'entity' and target_kind = 'task' and target_id = old.id)
begin
  select raise(abort, 'cannot change task scope with entity annotations; remove annotations explicitly first');
end;

-- A source UUID is created exactly once per database and never derived from
-- its pathname. Operation receipts are introduced with the model so a later
-- transactional writer cannot silently make entity notes non-recoverable.
create table annotation_source_identity (
  singleton integer primary key check (singleton = 1),
  source_uuid text not null unique,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
insert into annotation_source_identity(singleton, source_uuid)
values (1, lower(hex(randomblob(16))));

create table annotation_operation_receipts (
  operation_uuid text primary key,
  source_uuid text not null references annotation_source_identity(source_uuid),
  payload_digest text not null,
  annotation_id integer references annotations(id),
  revision integer,
  outcome text not null,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

insert into schema_migrations (version, description)
values (34, 'annotations: entity anchors, revisions, source identity, and operation receipts');
