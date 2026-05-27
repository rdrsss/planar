-- agent_work_claims: synchronization primitive. Answers "who owns this
-- work right now?" and is the source of truth for excluding in-flight
-- work from next-work selection. Exclusive vs shared is operator-
-- declared at claim time; exclusivity is enforced transactionally in
-- the store (SQLite cannot express the time-dependent "unexpired"
-- predicate in a partial unique index, so the store opens BEGIN
-- IMMEDIATE for acquire / heartbeat / force-takeover).
--
-- claim_token is a stable opaque handle (32-char lowercase hex of 16
-- random bytes; produced by randomblob(16) in SQL or std.crypto.random
-- in Zig). Clients must not parse semantics out.
--
-- worktree_id is intentionally NOT a schema-level foreign key — plan
-- 297's worktrees table may land before or after this migration, and
-- SQLite cannot add a foreign key without a table rebuild. The store
-- validates the id against worktrees at runtime when the table exists.
--
-- Locality columns (repo_root, branch, head_sha_at_claim, dirty_at_claim)
-- are best-effort snapshots of git state at claim time. NULL / unknown
-- when the probe was skipped or the cwd isn't a git checkout. The
-- viewer renders them; the engine does not gate on them.
create table agent_work_claims (
  id                integer primary key autoincrement,
  claim_token       text not null unique,
  session_id        integer not null references sessions(id) on delete cascade,
  entity_kind       text not null check(entity_kind in ('plan','plan_step','task')),
  entity_id         integer not null,
  claim_scope       text not null default 'exclusive' check(claim_scope in ('exclusive','shared')),
  status            text not null default 'active' check(status in (
    'active','released','completed','aborted','stale'
  )),
  vendor            text not null,
  vendor_session_id text,
  role              text,
  model             text,
  worktree_id       integer,
  worktree_path     text,
  repo_root         text,
  branch            text,
  head_sha_at_claim text,
  dirty_at_claim    text check(dirty_at_claim in ('clean','dirty','unknown')),
  purpose           text,
  base_ref          text,
  claimed_at        text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  last_heartbeat_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  lease_expires_at  text not null,
  released_at       text,
  release_reason    text,
  check (released_at is null or status in ('released','completed','aborted','stale'))
);

create index ix_agent_work_claims_session on agent_work_claims(session_id);
create index ix_agent_work_claims_entity on agent_work_claims(entity_kind, entity_id);
create index ix_agent_work_claims_active on agent_work_claims(entity_kind, entity_id, lease_expires_at) where status = 'active';
create index ix_agent_work_claims_vendor on agent_work_claims(vendor, vendor_session_id);
create index ix_agent_work_claims_heartbeat on agent_work_claims(last_heartbeat_at);
create index ix_agent_work_claims_worktree on agent_work_claims(worktree_id) where worktree_id is not null;

-- agent_actions: typed, time-bounded activity record. Answers "what
-- happened" and "what is running right now?". Bound to sessions for
-- the durable-handoff lineage. session_entry_id populated only for
-- actions an operator would expect in the session timeline (role
-- transitions, orchestrator dispatch); per-tool-call / per-heartbeat
-- rows leave it NULL to keep session_entries from fanning out to
-- per-event row counts. parent_action_id is acyclic by construction
-- (child rows reference an already-inserted parent's pk; the store API
-- does not expose updates to parent_action_id). claim_id links a sub-
-- action back to its owning agent_work_claim row.
--
-- Locality columns (head_sha, dirty) capture the git state at action
-- start so the viewer can show "agent X moved sha A -> B between two
-- actions". NULL when the action did not probe locality (heartbeat /
-- tool_call children typically skip the probe).
--
-- Declared AFTER agent_work_claims so the claim_id FK targets an
-- existing table.
create table agent_actions (
  id               integer primary key autoincrement,
  session_id       integer not null references sessions(id) on delete cascade,
  session_entry_id integer references session_entries(id) on delete set null,
  parent_action_id integer references agent_actions(id) on delete set null,
  claim_id         integer references agent_work_claims(id) on delete set null,
  action_kind      text not null check(action_kind in (
    'planner','ingestor','coder','test_coder','reviewer','ext_sync',
    'ext_propagate','orchestrator','resume','workbench_sync','spec_draft',
    'claim_check','heartbeat','tool_call','user_message','assistant_message',
    'other'
  )),
  entity_kind      text check(entity_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision'
  )),
  entity_id        integer,
  vendor           text not null,
  vendor_role      text,
  model            text,
  started_at       text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  ended_at         text,
  outcome          text check(outcome in ('ok','error','aborted','timeout')),
  summary          text,
  head_sha         text,
  dirty            text check(dirty in ('clean','dirty','unknown')),
  check (
    (entity_kind is null and entity_id is null) or
    (entity_kind is not null and entity_id is not null)
  )
);

create index ix_agent_actions_session on agent_actions(session_id);
create index ix_agent_actions_claim on agent_actions(claim_id) where claim_id is not null;
create index ix_agent_actions_entity on agent_actions(entity_kind, entity_id) where entity_kind is not null;
create index ix_agent_actions_active on agent_actions(started_at) where ended_at is null;
create index ix_agent_actions_kind on agent_actions(action_kind);
create index ix_agent_actions_started on agent_actions(started_at);
create index ix_agent_actions_parent on agent_actions(parent_action_id) where parent_action_id is not null;

insert into schema_migrations (version, description) values (15, 'agent activity tracking: agent_work_claims + agent_actions (with locality columns)');
