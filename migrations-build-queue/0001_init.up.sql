-- 0001 init: the build queue's own database (decisions 1430,
-- 1432 and 1440; tech spec 705 § Schema Changes). The queue moves out of
-- planar.db into planar-build-queue's database, with no data migration
-- (decision 1430). The queue is generic: it stores an opaque `tag` and
-- carries no vendor, role or claim token (decision 1432). The queue_entries
-- and queue_history columns are the planar.db queue tables (migrations
-- 00040 and 00042) minus vendor, role and claim_token, plus tag.
--
-- This file is the first and only migration in this chain. It creates the
-- schema_migrations table itself, because this chain's contract is its own
-- schema_migrations table (migrations-build-queue/README.md).

-- schema_migrations: this database's schema-version table. Same shape as
-- the planar.db contract (migrations/README.md), in a file of its own.
create table schema_migrations (
  version     integer primary key,
  applied_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  description text not null
);

-- queue_entries: commands that are waiting or running. One row per
-- submitted command that has not finished. The submitter (host_id, pid,
-- pid_started) owns the entry; the child group (child_pgid, child_started)
-- is the process group the command runs in once it has started. parent_seq
-- is set for a nested run. Monotonic columns (*_mono) are compared;
-- wall-clock columns (enqueued_at, started_at) are for display only.
-- run_limit_ms is the run limit deadline_mono was computed from (NULL while
-- waiting); wait_limit_ms is the --wait-timeout value (NULL when unlimited).
-- tag is the caller's opaque label for the entry (NULL when none).
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
  tag                    text,
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
-- removed and pruned after the retention period. seq is the ended entry's
-- sequence number; successor_seq names the entry a cancel re-queued in its
-- place, when one did. run_limit_ms and wait_limit_ms are copied from the
-- entry when it ends. tag is copied from the entry, as label is.
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
  tag           text,
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
create index idx_queue_history_tag on queue_history (tag);

-- queue_schema: the queue's own compatibility contract inside this
-- database. The row with the highest version is authoritative; compat is
-- the oldest queue version that may use queue_entries and queue_history.
-- The table keeps its planar.db name, so the queue_schema check reads it
-- the same way in both places.
create table queue_schema (
  version     integer primary key,
  compat      integer not null,
  description text    not null
);

insert into queue_schema (version, compat, description)
values (1, 1, 'queue_entries and queue_history in the build queue database');

-- queue_identity: names the database as a build-queue database. The
-- migration runner reads it to refuse the main chain against this file and
-- the build-queue chain against planar.db (migrations-build-queue/README.md).
create table queue_identity (
  kind       text primary key check (kind = 'planar-build-queue'),
  created_at text not null
);

insert into queue_identity (kind, created_at)
values ('planar-build-queue', strftime('%Y-%m-%dT%H:%M:%fZ','now'));

-- queue_settings: operator-set queue tunables, one row per key. The key
-- list is closed; a new tunable is a new migration.
create table queue_settings (
  key   text primary key check (key in (
    'slots', 'poll_interval', 'stale_after', 'grace', 'history_days', 'tagged_history_days'
  )),
  value text not null
);

-- The sequence floor: AUTOINCREMENT continues from this row, so the first
-- entry is 2000001. Numbers from the planar.db queue (floor 1000000, and
-- the retired agent.db below that) never collide with numbers from this
-- database. DROP TABLE removes the row again.
insert into sqlite_sequence (name, seq) values ('queue_entries', 2000000);

insert into schema_migrations (version, description)
values (1, 'build queue: queue_entries, queue_history, queue_schema, queue_identity, queue_settings, sequence floor 2000000');
