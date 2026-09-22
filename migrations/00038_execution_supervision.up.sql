-- 00038 execution_supervision — plan 1033 M0, task 6487 (decision 1007;
-- tech-spec D3 "the orchestrator creates the claim; the engine supervises and
-- terminates it", D4 "idempotency is keyed by Centurion attempt ID", D5
-- "explicit engine selector"). Named `00034_execution_supervision` in the plan;
-- 00034 was taken by entity_annotations before this landed.
--
-- Three changes, and nothing else:
--
--   agent_work_claims  + supervisor ('caller' | 'engine', default 'caller')
--                      + attempt_id (the supervising Centurion attempt, or null)
--   workflow_runs      + engine ('embedded' | 'centurion', default 'embedded')
--                      plan_id becomes nullable (a Centurion run need not be
--                      bound to a plan)
--   agent_actions      action_kind gains claim_associate, claim_terminal,
--                      supervisor_override, run_submitted, run_reconciled
--
-- Every pre-existing row keeps its meaning: an existing claim reads
-- supervisor = 'caller' with attempt_id null, and an existing run reads
-- engine = 'embedded'.
--
-- ## Why two constraints are relaxed by editing sqlite_schema, not by rebuild
--
-- SQLite cannot drop a NOT NULL or widen a CHECK with ALTER TABLE. The usual
-- answer is a table rebuild, and it is WRONG for these two tables, because
-- both are foreign-key parents (agent_work_claims.run_id and
-- context_records.run_id reference workflow_runs; agent_actions references
-- itself) and the runtime applies this file inside a transaction with foreign
-- keys ON, where `foreign_keys` cannot be switched off. Measured against a
-- seeded database before this file was written:
--
--   * rename-old / create-new / drop-old rewrites every child's REFERENCES
--     clause to the `_old` table (even under legacy_alter_table, which only
--     applies when foreign keys are off), and dropping it then CASCADE-deleted
--     every context_records row and NULLed every claim's run_id.
--   * create-new / drop-old / rename-new fires the same ON DELETE actions from
--     DROP TABLE's implicit DELETE.
--
-- SQLite documents the alternative for exactly this class of change — ones
-- that leave the stored rows valid, such as removing a NOT NULL or relaxing a
-- CHECK (lang_altertable.html § "Making Other Kinds Of Table Schema
-- Changes"): edit the table's CREATE text in sqlite_schema under
-- `writable_schema`, then make the connection re-read it. No table is dropped,
-- so no foreign-key action can fire and no row moves. Both steps are legal in
-- a transaction, and a failure anywhere rolls the whole file back.
--
-- The edits are exact-text replacements of what migrations 00015 and 00022
-- created. The guard after them fails the migration by name if either
-- replacement did not take (a database whose stored text differs), rather
-- than recording version 38 over an unchanged constraint.

-- agent_work_claims: added columns suffice.
alter table agent_work_claims
add column supervisor text not null default 'caller' check (supervisor in ('caller', 'engine'));

alter table agent_work_claims add column attempt_id text;

create index ix_agent_work_claims_attempt on agent_work_claims (attempt_id)
where attempt_id is not null;

-- workflow_runs: the engine column is added BEFORE the text edits. ALTER
-- TABLE ADD COLUMN splices into the stored text at an offset remembered from
-- the connection's last parse, so it must not follow an edit it has not seen.
alter table workflow_runs
add column engine text not null default 'embedded' check (engine in ('embedded', 'centurion'));

pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  'plan_id       integer not null references plans(id)',
  'plan_id       integer references plans(id)'
)
where type = 'table' and name = 'workflow_runs';

update sqlite_schema
set sql = replace(
  sql,
  '''assistant_message'',
    ''other''
  ))',
  '''assistant_message'',
    ''other'',
    ''claim_associate'',''claim_terminal'',''supervisor_override'',
    ''run_submitted'',''run_reconciled''
  ))'
)
where type = 'table' and name = 'agent_actions';

-- Clears the flag AND makes this connection re-read sqlite_schema, so the
-- relaxed constraints hold for every statement after this one, including a
-- later migration in the same `apply_all` pass.
pragma writable_schema = reset;

-- Fail by name if either edit did not land. TEMP, so it never touches the
-- database file; dropped on success, rolled back with everything else on
-- failure.
create temp table m00038_up_guard (
  refusal text
    constraint m00038_up_refused_unexpected_stored_schema_text_plan_id_or_action_kind_not_relaxed
    check (refusal is null)
);
insert into temp.m00038_up_guard (refusal)
select 'unrelaxed'
where not exists (
  select 1 from sqlite_schema
  where type = 'table' and name = 'workflow_runs' and sql like '%plan_id       integer references plans(id)%'
)
or not exists (
  select 1 from sqlite_schema
  where type = 'table' and name = 'agent_actions' and sql like '%''run_reconciled''%'
);
drop table temp.m00038_up_guard;

insert into schema_migrations (version, description)
values (38, 'execution supervision: claim supervisor/attempt, run engine, nullable run plan, supervision action kinds');
