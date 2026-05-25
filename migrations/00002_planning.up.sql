

-- ============================================================
-- Plans
-- ============================================================

-- plans: top-level structured intent for a body of work. May be hierarchical via
-- parent_plan_id. Lifecycle: draft -> active -> done (or paused/abandoned). Tasks
-- and plan_steps hang off a plan. slug is a filesystem-safe identifier unique
-- per parent scope, enforced by three partial unique indexes (see file header).
create table plans (
  id             integer primary key autoincrement,
  scope_kind     text not null check(scope_kind in ('repo','association','global')),
  scope_id       integer,
  title          text not null,
  slug           text not null,
  summary        text,
  status         text not null default 'draft' check(status in ('draft','active','paused','done','abandoned')),
  parent_plan_id integer references plans(id) on delete set null,
  created_at     text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at     text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (scope_kind = 'global' and scope_id is null)
    or (scope_kind in ('repo','association') and scope_id is not null)
  )
);

create index ix_plans_scope on plans(scope_kind, scope_id);
create index ix_plans_status on plans(status);
create index ix_plans_parent on plans(parent_plan_id) where parent_plan_id is not null;

-- Slug uniqueness: three partial indexes rather than a table-level UNIQUE.
-- See file header for the rationale (SQLite NULL UNIQUE semantics).
create unique index ux_plans_top_slug_global on plans(scope_kind, slug) where parent_plan_id is null and scope_id is null;
create unique index ux_plans_top_slug_scoped on plans(scope_kind, scope_id, slug) where parent_plan_id is null and scope_id is not null;
create unique index ux_plans_child_slug on plans(parent_plan_id, slug) where parent_plan_id is not null;

-- ============================================================
-- Artifacts
-- ============================================================

-- artifacts: tech specs, ADRs, design notes, generated summaries, READMEs,
-- product specs, and roadmaps. The durable documents that crystallize from work.
-- Scope-aware so they can be repo-specific, association-shared, or personal-global.
create table artifacts (
  id          integer primary key autoincrement,
  scope_kind  text not null check(scope_kind in ('repo','association','global')),
  scope_id    integer,
  kind        text not null check(kind in ('tech_spec','adr','design_note','summary','readme','generated','other','product_spec','roadmap')),
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

create index ix_artifacts_scope on artifacts(scope_kind, scope_id);
create index ix_artifacts_kind on artifacts(kind);
create index ix_artifacts_status on artifacts(status);
create index ix_artifacts_source_path on artifacts(source_path) where source_path is not null;

-- ============================================================
-- Decisions
-- ============================================================

-- decisions: recorded decisions with rationale, scope-aware. Lifecycle: proposed
-- -> accepted -> superseded/withdrawn. Optionally linked to the session that
-- produced the decision for audit traceability. FK to sessions(id) is declared
-- here; sessions are created in 0005_sessions.sql — SQLite checks FK validity
-- at DML time (not DDL time), so this declaration is safe.
create table decisions (
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

create index ix_decisions_scope on decisions(scope_kind, scope_id);
create index ix_decisions_status on decisions(status);
create index ix_decisions_session on decisions(session_id) where session_id is not null;

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (2, 'planning pipeline: workbench sync, plan slugs, cross-repo links, association config');
