-- Entity annotations and operation receipts cannot be represented by the
-- previous schema. Export them before rollback; this guard deliberately
-- refuses loss rather than detaching notes or reopening receipt replay.
create table annotation_rollback_guard (value integer check (value = 0));
insert into annotation_rollback_guard(value)
select 1 where exists (select 1 from annotations where anchor_kind = 'entity')
   or exists (select 1 from annotation_operation_receipts);
drop table annotation_rollback_guard;

drop trigger if exists plans_reject_entity_annotation_delete;
drop trigger if exists tasks_reject_entity_annotation_delete;
drop trigger if exists annotations_entity_target_insert;
drop trigger if exists annotations_entity_target_update;
drop trigger if exists annotations_search_insert;
drop trigger if exists annotations_search_update;
drop trigger if exists annotations_search_delete;

drop table annotation_operation_receipts;
drop table annotation_source_identity;

create table annotations_old (
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
  status            text    not null default 'active' check (status in ('active','resolved','dismissed','archived')),
  vendor            text    not null default '',
  plan_id           integer references plans(id) on delete set null,
  task_id           integer references tasks(id) on delete set null,
  created_at        text    not null,
  updated_at        text    not null,
  check ((scope_kind = 'global' and scope_id is null)
      or (scope_kind in ('repo','association') and scope_id is not null))
);
insert into annotations_old select id, scope_kind, scope_id, anchor_path,
  anchor_line_start, anchor_line_end, anchor_commit_sha, anchor_text_hash,
  anchor_text, title, slug, body, status, vendor, plan_id, task_id,
  created_at, updated_at from annotations;
drop table annotations;
alter table annotations_old rename to annotations;
create index ix_annotations_scope on annotations(scope_kind, scope_id);
create index ix_annotations_anchor_path on annotations(anchor_path);
create index ix_annotations_plan_id on annotations(plan_id) where plan_id is not null;
create index ix_annotations_task_id on annotations(task_id) where task_id is not null;
create index ix_annotations_status on annotations(status);
create index ix_annotations_created_at on annotations(created_at);
create unique index ux_annotations_slug on annotations(slug) where slug is not null;
create trigger annotations_search_insert after insert on annotations begin
  insert into search_annotations(rowid, title, body) values (new.id, coalesce(new.title, ''), new.body);
end;
create trigger annotations_search_delete after delete on annotations begin
  delete from search_annotations where rowid = old.id;
end;
create trigger annotations_search_update after update on annotations begin
  delete from search_annotations where rowid = old.id;
  insert into search_annotations(rowid, title, body) values (new.id, coalesce(new.title, ''), new.body);
end;
delete from schema_migrations where version = 34;
