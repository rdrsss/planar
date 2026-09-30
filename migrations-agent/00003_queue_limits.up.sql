-- ============================================================
-- Host-wide build and test queue: the limits an entry was submitted with
-- (plan 1080, task hq-queue-limit-columns; tech spec 647 § CLI surface,
-- `queue status --json` fields `run_limit_ms` and `wait_limit_ms`)
-- ============================================================

-- queue_entries and queue_history record deadlines on a monotonic clock
-- (`deadline_mono`, `wait_deadline_mono`), from which the limit an entry
-- was submitted with cannot be recovered. These columns record the limits
-- themselves, in milliseconds, so `queue status` can report them.
--
-- wait_limit_ms: the `--wait-timeout` value, written at enqueue; NULL when
-- the entry has no wait limit.
-- run_limit_ms: the run limit the entry's `deadline_mono` was computed from,
-- written in the same statement that starts the entry (the poll's turn, or
-- a nested run's insert); NULL while the entry is waiting.
-- The history row copies both from the entry when it ends.
--
-- Both are nullable with no default, and every existing row reads NULL,
-- which is what an older store honestly knows.
alter table queue_entries add column run_limit_ms integer;
alter table queue_entries add column wait_limit_ms integer;

alter table queue_history add column run_limit_ms integer;
alter table queue_history add column wait_limit_ms integer;

-- ============================================================
-- Record the migration
-- ============================================================

-- Additive only (four nullable columns an older binary never names), so
-- compat stays at the previous migration's value: a planar-agent built at
-- agent schema version 1 or 2 may still open this store, and its inserts
-- leave the new columns NULL.
insert into agent_schema_migrations (version, compat, description)
values (3, 1, 'queue_entries and queue_history record run and wait limits');
