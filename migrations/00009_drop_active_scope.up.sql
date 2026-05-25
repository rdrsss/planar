-- 0009_drop_active_scope.sql
-- Drop the active_scope table. Plan 153 M4 made cwd derivation the primary
-- signal for read-side scope resolution; M5 (this migration plus the
-- accompanying code change) removes the stack consultation from the
-- write-side resolver and deletes the `scope use|pop|clear` verbs. With
-- nothing left in the binary that reads or writes active_scope, the table
-- becomes dead weight and is dropped here.
--
-- A future contributor seeing rows in active_scope would be tempted to
-- reintroduce stack-consultation; removing the table closes that door
-- entirely. See artifact 94 (tech spec) §"Why drop the active_scope table
-- on disk rather than ignore it" for the full reasoning.

drop table if exists active_scope;

insert into schema_migrations (version, description)
values (9, 'drop active_scope table — cwd-primary scope resolution (plan 153)');
