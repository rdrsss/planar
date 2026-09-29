-- ============================================================
-- Host-wide build and test queue (plan 1080, decisions 1178, 1181, 1182,
-- 1199; tech spec 647 § Schema Changes)
-- ============================================================

-- queue_entries: commands that are waiting or running. One row per
-- submitted command that has not finished. The submitter (host_id, pid,
-- pid_started) owns the entry; the child group (child_pgid, child_started)
-- is the process group the command runs in once it has started. parent_seq
-- is set for a nested run. Monotonic columns (*_mono) are compared;
-- wall-clock columns (enqueued_at, started_at) are for display only.
create table queue_entries (
  seq                    integer primary key autoincrement,
  state                  text    not null check(state in ('waiting', 'running')),
  host_id                text    not null,
  pid                    integer not null,
  pid_started            integer not null,
  child_pgid             integer,
  child_started          integer,
  parent_seq             integer,
  terminating_since_mono integer,
  terminate_reason       text    check(terminate_reason in ('timeout', 'cancelled')),
  cancelled_by           text,
  cwd                    text    not null,
  argv                   text    not null,
  label                  text,
  vendor                 text,
  role                   text,
  claim_token            text,
  log_path               text,
  enqueued_at            integer not null,
  started_at             integer,
  refreshed_mono         integer not null,
  deadline_mono          integer,
  wait_deadline_mono     integer
);
create index idx_queue_entries_state on queue_entries(state);
create index idx_queue_entries_parent_seq on queue_entries(parent_seq);

-- queue_history: one row per entry that has ended, kept after the entry is
-- removed and pruned after the retention period (decision 1199). seq is the
-- ended entry's sequence number; successor_seq names the entry a cancel
-- re-queued in its place, when one did.
create table queue_history (
  seq           integer primary key,
  outcome       text    not null check(outcome in ('exited', 'signaled', 'timeout', 'cancelled', 'wait_timeout', 'not_started', 'abandoned')),
  exit_code     integer,
  signal        integer,
  successor_seq integer,
  cancelled_by  text,
  nested        integer not null default 0,
  parent_seq    integer,
  cwd           text    not null,
  argv          text    not null,
  label         text,
  vendor        text,
  role          text,
  log_path      text,
  enqueued_at   integer not null,
  started_at    integer,
  ended_at      integer not null,
  waited_ms     integer not null,
  ran_ms        integer
);
create index idx_queue_history_ended_at on queue_history(ended_at);

-- ============================================================
-- Record the migration
-- ============================================================

-- Additive only (two new tables and their indexes), so compat stays at the
-- previous migration's value: a planar-agent built at agent schema version
-- 1 may still open this store.
insert into agent_schema_migrations (version, compat, description)
values (2, 1, 'queue_entries and queue_history');
