-- Rolls back 00011_slug_refs_fts.up.sql


-- Drop triggers (named explicitly so the Down is auditable).
drop trigger if exists plans_fts_ai;
drop trigger if exists plans_fts_au;
drop trigger if exists plans_fts_ad;
drop trigger if exists tasks_fts_ai;
drop trigger if exists tasks_fts_au;
drop trigger if exists tasks_fts_ad;
drop trigger if exists questions_fts_ai;
drop trigger if exists questions_fts_au;
drop trigger if exists questions_fts_ad;
drop trigger if exists test_scenarios_fts_ai;
drop trigger if exists test_scenarios_fts_au;
drop trigger if exists test_scenarios_fts_ad;
drop trigger if exists decisions_fts_ai;
drop trigger if exists decisions_fts_au;
drop trigger if exists decisions_fts_ad;
drop trigger if exists artifacts_fts_ai;
drop trigger if exists artifacts_fts_au;
drop trigger if exists artifacts_fts_ad;

-- Drop FTS virtual tables.
drop table if exists plans_fts;
drop table if exists tasks_fts;
drop table if exists questions_fts;
drop table if exists test_scenarios_fts;
drop table if exists decisions_fts;
drop table if exists artifacts_fts;

-- Drop per-kind slug unique indexes (the column drop below would do this
-- implicitly during the table-rebuild but the explicit drops make the Down
-- direction self-documenting).
drop index if exists ux_artifacts_slug;
drop index if exists ux_tasks_slug;
drop index if exists ux_questions_slug;
drop index if exists ux_test_scenarios_slug;
drop index if exists ux_decisions_slug;

-- ============================================================
-- Table-rebuild to drop the slug column from each affected table
-- ============================================================
--
-- SQLite ≥ 3.35 supports ALTER TABLE DROP COLUMN, but only on tables with
-- no constraints referencing the dropped column. Rather than rely on the
-- version of SQLite linked into modernc.org/sqlite at any future date, we
-- use the canonical create-new + copy + drop + rename pattern matching
-- 0008_doc_artifact_kinds.sql. Each rebuild also re-creates the same
-- indexes the original table had.

-- artifacts ------------------------------------------------------------
create table artifacts_old (
  id          integer primary key autoincrement,
  scope_kind  text not null check(scope_kind in ('repo','association','global')),
  scope_id    integer,
  kind        text not null check(kind in (
    'tech_spec','adr','design_note','summary','readme','generated','other',
    'product_spec','roadmap',
    'research','getting_started','changelog_entry','glossary_term'
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

-- tasks ----------------------------------------------------------------
create table tasks_old (
  id             integer primary key autoincrement,
  scope_kind     text not null check(scope_kind in ('repo','association','global')),
  scope_id       integer,
  plan_id        integer references plans(id) on delete set null,
  parent_task_id integer references tasks(id) on delete set null,
  title          text not null,
  body           text,
  status         text not null default 'todo' check(status in ('todo','doing','blocked','done','cancelled')),
  priority       integer not null default 100,
  next_action    text,
  due_at         text,
  created_at     text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at     text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);
insert into tasks_old
  select id, scope_kind, scope_id, plan_id, parent_task_id, title, body, status, priority, next_action, due_at, created_at, updated_at
  from tasks;
drop table tasks;
alter table tasks_old rename to tasks;
create index ix_tasks_scope on tasks(scope_kind, scope_id);
create index ix_tasks_status on tasks(status);
create index ix_tasks_plan on tasks(plan_id) where plan_id is not null;
create index ix_tasks_parent on tasks(parent_task_id) where parent_task_id is not null;
create index ix_tasks_open_priority on tasks(priority, updated_at) where status in ('todo','doing','blocked');
create index ix_tasks_due on tasks(due_at) where due_at is not null;

-- questions ------------------------------------------------------------
create table questions_old (
  id          integer primary key autoincrement,
  scope_kind  text not null check(scope_kind in ('repo','association','global')),
  scope_id    integer,
  title       text not null,
  body        text,
  status      text not null default 'open' check(status in ('open','answered','wontfix')),
  answer_body text,
  answered_at text,
  created_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  ),
  check (
    (status = 'answered' and answer_body is not null and answered_at is not null)
    or (status != 'answered')
  )
);
insert into questions_old
  select id, scope_kind, scope_id, title, body, status, answer_body, answered_at, created_at, updated_at
  from questions;
drop table questions;
alter table questions_old rename to questions;
create index ix_questions_scope on questions(scope_kind, scope_id);
create index ix_questions_status on questions(status);
create index ix_questions_open on questions(updated_at) where status = 'open';

-- test_scenarios -------------------------------------------------------
create table test_scenarios_old (
  id                  integer primary key autoincrement,
  scope_kind          text not null check(scope_kind in ('repo','association','global')),
  scope_id            integer,
  title               text not null,
  body                text,
  status              text not null default 'draft' check(status in ('draft','ready','verified','failing','retired')),
  related_artifact_id integer references artifacts(id) on delete set null,
  last_run_at         text,
  last_outcome        text check(last_outcome in ('pass','fail','error','skipped')),
  created_at          text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at          text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);
insert into test_scenarios_old
  select id, scope_kind, scope_id, title, body, status, related_artifact_id, last_run_at, last_outcome, created_at, updated_at
  from test_scenarios;
drop table test_scenarios;
alter table test_scenarios_old rename to test_scenarios;
create index ix_test_scenarios_scope on test_scenarios(scope_kind, scope_id);
create index ix_test_scenarios_status on test_scenarios(status);
create index ix_test_scenarios_artifact on test_scenarios(related_artifact_id) where related_artifact_id is not null;

-- decisions ------------------------------------------------------------
create table decisions_old (
  id         integer primary key autoincrement,
  scope_kind text not null check(scope_kind in ('repo','association','global')),
  scope_id   integer,
  title      text not null,
  body       text not null,
  rationale  text,
  status     text not null default 'proposed' check(status in ('proposed','accepted','superseded','withdrawn')),
  decided_at text,
  session_id integer references sessions(id) on delete set null,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);
insert into decisions_old
  select id, scope_kind, scope_id, title, body, rationale, status, decided_at, session_id, created_at, updated_at
  from decisions;
drop table decisions;
alter table decisions_old rename to decisions;
create index ix_decisions_scope on decisions(scope_kind, scope_id);
create index ix_decisions_status on decisions(status);
create index ix_decisions_session on decisions(session_id) where session_id is not null;

delete from schema_migrations where version = 11;
