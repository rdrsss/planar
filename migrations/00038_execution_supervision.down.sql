-- 00038 execution_supervision, reversed. See the up file for why the two
-- constraint changes are sqlite_schema text edits rather than table rebuilds.
--
-- ## The guard runs first and refuses by name
--
-- Two kinds of row cannot be represented by the older schema: a run with a
-- null plan_id (plan_id was NOT NULL) and an action whose kind this migration
-- added. Rollback refuses rather than inventing a plan or deleting history.
-- It matters more here than for an ordinary rollback: restoring a constraint
-- by editing its text does NOT re-check existing rows, so without this guard
-- a rollback would silently leave rows that violate the restored schema.
--
-- `rollback_all` does not wrap a down migration in a transaction, so the
-- guard must change nothing when it fires: the only write is into a TEMP
-- table, whose named CHECK constraint turns a refusal into an error that says
-- why ("CHECK constraint failed: m00038_down_refused_..."). The database file
-- is untouched and the rollback can be retried once the rows are disposed
-- of. Dropping supervisor/attempt_id and engine loses nothing the older
-- schema's readers could interpret, so those do not block.

create temp table if not exists m00038_down_guard (
  refusal text
    constraint m00038_down_refused_null_plan_run_or_supervision_action_kind_present
    check (refusal is null)
);
insert into temp.m00038_down_guard (refusal)
select 'blocked'
where exists (select 1 from workflow_runs where plan_id is null)
   or exists (
     select 1 from agent_actions
     where action_kind in ('claim_associate', 'claim_terminal', 'supervisor_override', 'run_submitted', 'run_reconciled')
   );
drop table temp.m00038_down_guard;

-- Columns first: DROP COLUMN, like ADD COLUMN, rewrites the stored text from
-- the connection's last parse, so it must run before the text edits below.
drop index ix_agent_work_claims_attempt;

alter table agent_work_claims drop column attempt_id;

alter table agent_work_claims drop column supervisor;

alter table workflow_runs drop column engine;

pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  'plan_id       integer references plans(id)',
  'plan_id       integer not null references plans(id)'
)
where type = 'table' and name = 'workflow_runs';

update sqlite_schema
set sql = replace(
  sql,
  '''assistant_message'',
    ''other'',
    ''claim_associate'',''claim_terminal'',''supervisor_override'',
    ''run_submitted'',''run_reconciled''
  ))',
  '''assistant_message'',
    ''other''
  ))'
)
where type = 'table' and name = 'agent_actions';

pragma writable_schema = reset;

delete from schema_migrations where version = 38;
