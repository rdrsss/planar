

-- ============================================================
-- Sessions
-- ============================================================

-- sessions: one vendor's episode of work, scoped to a task and project. The
-- vendor, vendor_session_id, and model columns make sessions self-describing for
-- handoff — there is no separate agent_sessions indirection. ended_at NULL means
-- the session is still active.
create table sessions (
  id                integer primary key autoincrement,
  task_id           integer references tasks(id) on delete set null,
  project_id        integer references projects(id) on delete set null,
  agent_id          integer references agents(id) on delete set null,
  vendor            text not null,
  vendor_session_id text,
  model             text,
  started_at        text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  ended_at          text,
  summary           text,
  unique (vendor, vendor_session_id)
);

create index ix_sessions_task on sessions(task_id) where task_id is not null;
create index ix_sessions_project on sessions(project_id) where project_id is not null;
create index ix_sessions_agent on sessions(agent_id) where agent_id is not null;
create index ix_sessions_vendor on sessions(vendor);
create index ix_sessions_started on sessions(started_at);
create index ix_sessions_active on sessions(started_at) where ended_at is null;

-- ============================================================
-- Session entries
-- ============================================================

-- session_entries: timeline within a session. Each row is a typed event
-- (action, observation, decision, question, file, command, note, error, read)
-- with a body. The 'read' prefix labels read-only ingestor preview entries
-- (spec ingest without --apply). Ordered by `ordinal` within a session.
create table session_entries (
  id         integer primary key autoincrement,
  session_id integer not null references sessions(id) on delete cascade,
  ordinal    integer not null,
  prefix     text not null check(prefix in ('action','observation','decision','question','file','command','note','error','read')),
  body       text not null,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (session_id, ordinal)
);

create index ix_session_entries_session on session_entries(session_id);
create index ix_session_entries_prefix on session_entries(prefix);

-- ============================================================
-- Context snapshots
-- ============================================================

-- context_snapshots: the resume packet payload, produced at terminate time or on
-- demand. Carries a narrative body and an exact next_action, plus the vendor
-- identity needed for cross-vendor handoff. Consumed by `planar resume`.
create table context_snapshots (
  id                integer primary key autoincrement,
  session_id        integer not null references sessions(id) on delete cascade,
  task_id           integer references tasks(id) on delete set null,
  vendor            text not null,
  vendor_session_id text,
  body              text,
  next_action       text,
  created_at        text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_context_snapshots_session on context_snapshots(session_id);
create index ix_context_snapshots_task on context_snapshots(task_id) where task_id is not null;
create index ix_context_snapshots_vendor on context_snapshots(vendor);
create index ix_context_snapshots_created on context_snapshots(created_at);

-- ============================================================
-- Handoffs
-- ============================================================

-- handoffs: explicit transition from a snapshot to the next session, with from/to
-- vendor labels. Status (pending -> validated -> consumed, or abandoned) tracks
-- where the handoff is in its lifecycle. Cross-vendor resume is the same code
-- path with different vendor strings on the source and destination.
create table handoffs (
  id               integer primary key autoincrement,
  from_snapshot_id integer not null references context_snapshots(id) on delete cascade,
  to_session_id    integer references sessions(id) on delete set null,
  from_vendor      text not null,
  to_vendor        text,
  status           text not null default 'pending' check(status in ('pending','validated','consumed','abandoned')),
  validated_at     text,
  consumed_at      text,
  created_at       text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_handoffs_snapshot on handoffs(from_snapshot_id);
create index ix_handoffs_to_session on handoffs(to_session_id) where to_session_id is not null;
create index ix_handoffs_status on handoffs(status);
create index ix_handoffs_pending on handoffs(created_at) where status = 'pending';

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (5, 'session_entries: add read prefix for read-only verbs');
