

-- ============================================================
-- Agents (registry of logical actors: agent specs, humans, vendors)
-- ============================================================

-- agents: registry of logical actors that produce sessions — Planar agent
-- role specs (worker, reviewer, ...), human users, and vendor identities. Sessions
-- reference this to record who did the work.
create table agents (
  id         integer primary key autoincrement,
  name       text not null unique,
  kind       text not null check(kind in ('planar-agent','human','vendor')),
  role       text,
  tier       text check(tier in ('medium','large')),
  spec_path  text,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_agents_kind on agents(kind);
create index ix_agents_role on agents(role) where role is not null;

-- ============================================================
-- Active scope
-- ============================================================

-- active_scope: ordered stack of associations currently in active scope. Drives
-- default filtering on `task list`, `plan show`, etc., and where new entities
-- land when scope is unspecified. Set explicitly via `planar scope use`.
create table active_scope (
  position       integer primary key,
  association_id integer not null references associations(id) on delete cascade,
  set_at         text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_active_scope_association on active_scope(association_id);

-- ============================================================
-- Tasks
-- ============================================================

-- tasks: a discrete unit of work. May hang off a plan (plan_id) or another task
-- (parent_task_id). next_action is required for handoff — `resume validate`
-- refuses a packet without it.
create table tasks (
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

create index ix_tasks_scope on tasks(scope_kind, scope_id);
create index ix_tasks_status on tasks(status);
create index ix_tasks_plan on tasks(plan_id) where plan_id is not null;
create index ix_tasks_parent on tasks(parent_task_id) where parent_task_id is not null;
create index ix_tasks_open_priority on tasks(priority, updated_at) where status in ('todo','doing','blocked');
create index ix_tasks_due on tasks(due_at) where due_at is not null;

-- ============================================================
-- Questions
-- ============================================================

-- questions: open questions surfaced during work. The CHECK constraint enforces
-- that status='answered' requires both answer_body and answered_at to be set —
-- "answered" without an answer is a contradiction we refuse to store.
create table questions (
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

create index ix_questions_scope on questions(scope_kind, scope_id);
create index ix_questions_status on questions(status);
create index ix_questions_open on questions(updated_at) where status = 'open';

-- ============================================================
-- Test scenarios
-- ============================================================

-- test_scenarios: verification scenarios attached to a spec, plan, or task.
-- Records the last run timestamp and outcome but does not execute anything —
-- execution remains the agent's job per the project's non-goals.
create table test_scenarios (
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

create index ix_test_scenarios_scope on test_scenarios(scope_kind, scope_id);
create index ix_test_scenarios_status on test_scenarios(status);
create index ix_test_scenarios_artifact on test_scenarios(related_artifact_id) where related_artifact_id is not null;

-- ============================================================
-- Plan steps (after tasks — FKs into tasks(id))
-- ============================================================

-- plan_steps: ordered breakdown of a plan into individual steps. The common case
-- is one step -> one task (plan_steps.task_id); richer relationships (one step ->
-- multiple tasks, blocking, supersession) go through entity_links.
create table plan_steps (
  id         integer primary key autoincrement,
  plan_id    integer not null references plans(id) on delete cascade,
  ordinal    integer not null,
  body       text not null,
  status     text not null default 'pending' check(status in ('pending','in-progress','done','skipped')),
  task_id    integer references tasks(id) on delete set null,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (plan_id, ordinal)
);

create index ix_plan_steps_plan on plan_steps(plan_id);
create index ix_plan_steps_task on plan_steps(task_id) where task_id is not null;
create index ix_plan_steps_status on plan_steps(status);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (3, 'workbench conflict persistence: nullable link_id, scope column, context_json');
