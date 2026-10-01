-- 00040 host_queue, reversed.
--
-- ## The guard runs first and refuses by name
--
-- A row in queue_entries is a command that is waiting or running right now:
-- its submitter is polling this table, and its child group may still be
-- running. Dropping the table under it would orphan that command outside any
-- queue. Rollback therefore refuses while queue_entries has a row; drain the
-- queue or cancel the entries first (migrations/README.md § Host-queue
-- rollback recovery). queue_history does not block: it is a record of runs
-- that have ended, and the rollback drops it.
--
-- `rollback_all` does not wrap a down migration in a transaction, so the
-- guard must change nothing when it fires: the only write is into a TEMP
-- table, whose named CHECK constraint turns a refusal into an error that says
-- why ("CHECK constraint failed: m00040_down_refused_live_queue_entries"),
-- the 00038 pattern. The database file is untouched and the rollback can be
-- retried once the queue is empty.
--
-- Dropping queue_entries also removes its sqlite_sequence row, so a later
-- re-apply starts again at the 1000000 floor. Detached logs numbered above
-- it may then collide; the same README section names the helper that
-- archives them.

create temp table if not exists m00040_down_guard (
  refusal text
    constraint m00040_down_refused_live_queue_entries
    check (refusal is null)
);
insert into temp.m00040_down_guard (refusal)
select 'blocked'
where exists (select 1 from queue_entries);
drop table temp.m00040_down_guard;

drop table queue_schema;

drop table queue_history;

drop table queue_entries;

delete from schema_migrations where version = 40;
