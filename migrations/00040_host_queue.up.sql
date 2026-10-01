-- 00040 host_queue: the host-wide build and test queue moves into
-- planar.db (plan 1089, decisions 1219, 1220 and 1222; tech spec 656
-- § Schema Changes). The two tables are the agent database's shape at agent
-- migration 00003 (migrations-agent/00002 plus 00003's two limit columns),
-- created here in their final column order. No row is imported from
-- agent.db: existing queue state is discarded (decision 1222).
--
-- The queue stays a separable unit inside planar.db: no foreign key, trigger
-- or view joins these tables to a planning table, and no transaction writes
-- both. The queue tables carry their own compatibility marker, queue_schema,
-- which the queue verbs read instead of refusing a planar.db that is ahead of
-- them (planar.engine.hostqueue.schema). Every later migration that names
-- queue_entries, queue_history or queue_schema must insert a queue_schema row
-- (contract in docs/architecture.md § The host queue's tables).

-- queue_entries: commands that are waiting or running. One row per
-- submitted command that has not finished. The submitter (host_id, pid,
-- pid_started) owns the entry; the child group (child_pgid, child_started)
-- is the process group the command runs in once it has started. parent_seq
-- is set for a nested run. Monotonic columns (*_mono) are compared;
-- wall-clock columns (enqueued_at, started_at) are for display only.
-- run_limit_ms is the run limit deadline_mono was computed from (NULL while
-- waiting); wait_limit_ms is the --wait-timeout value (NULL when unlimited).
create table queue_entries (
  seq                    integer primary key autoincrement,
  state                  text    not null check (state in ('waiting', 'running')),
  host_id                text    not null,
  pid                    integer not null,
  pid_started            integer not null,
  child_pgid             integer,
  child_started          integer,
  parent_seq             integer,
  terminating_since_mono integer,
  terminate_reason       text    check (terminate_reason in ('timeout', 'cancelled')),
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
  wait_deadline_mono     integer,
  run_limit_ms           integer,
  wait_limit_ms          integer
);
create index idx_queue_entries_state on queue_entries (state);
create index idx_queue_entries_parent_seq on queue_entries (parent_seq);

-- queue_history: one row per entry that has ended, kept after the entry is
-- removed and pruned after the retention period (decision 1199). seq is the
-- ended entry's sequence number; successor_seq names the entry a cancel
-- re-queued in its place, when one did. run_limit_ms and wait_limit_ms are
-- copied from the entry when it ends.
create table queue_history (
  seq           integer primary key,
  outcome       text    not null check (
    outcome in ('exited', 'signaled', 'timeout', 'cancelled', 'wait_timeout', 'not_started', 'abandoned')
  ),
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
  ran_ms        integer,
  run_limit_ms  integer,
  wait_limit_ms integer
);
create index idx_queue_history_ended_at on queue_history (ended_at);

-- queue_schema: the queue tables' own compatibility contract inside
-- planar.db. The row with the highest version is authoritative; compat is
-- the oldest queue version that may use queue_entries and queue_history.
-- A queue verb accepts a planar.db that is ahead of its binary when this
-- marker's compat is at most the binary's queue version and every column the
-- binary names exists (decision 1220).
create table queue_schema (
  version     integer primary key,
  compat      integer not null,
  description text    not null
);

insert into queue_schema (version, compat, description)
values (1, 1, 'queue_entries and queue_history in planar.db');

-- The sequence floor (decision 1222): AUTOINCREMENT continues from this row,
-- so the first entry is 1000001. Numbers from the retired agent.db (always
-- below 1000000) never collide with new ones, a leftover
-- PLANAR_QUEUE_SLOT=<old seq> names no new entry, and the installer can tell
-- old logs from new ones by number. DROP TABLE removes the row again.
insert into sqlite_sequence (name, seq) values ('queue_entries', 1000000);

insert into schema_migrations (version, description)
values (40, 'host queue: queue_entries, queue_history, queue_schema marker, sequence floor 1000000');
