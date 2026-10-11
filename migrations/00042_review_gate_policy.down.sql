-- 00042 review_gate_policy, reversed. See the up file for why the action_kind
-- change is a sqlite_schema text edit rather than a table rebuild.
--
-- ## Review actions are deleted, not refused
--
-- The older schema cannot hold an agent_actions row of kind 'review', and
-- restoring a CHECK by editing its text does not re-check existing rows. This
-- rollback DELETES every review row first rather than refusing, so a rollback
-- never stalls on review history: the verdicts are dropped with the verdict
-- column anyway. A child action that named a deleted review as its parent has
-- its parent_action_id set to null, as ON DELETE SET NULL would. Export
-- the review rows first if you need them (migrations/README.md § Review-gate
-- rollback).
--
-- policy_digest and claim_token are dropped with their values: the older
-- schema's readers never interpret them.
--
-- `rollback_all` does not wrap a down migration in a transaction, so nothing
-- here can refuse: there is no guard, and the delete runs first so that no
-- later statement meets a row the older schema rejects.

-- Spelled out rather than left to ON DELETE SET NULL, so a connection with
-- foreign keys off (a hand-run rollback) leaves no dangling parent either.
update agent_actions set parent_action_id = null
where parent_action_id in (select id from agent_actions where action_kind = 'review');

delete from agent_actions where action_kind = 'review';

-- Columns first: DROP COLUMN, like ADD COLUMN, rewrites the stored text from
-- the connection's last parse, so it must run before the text edit below.
drop index idx_queue_history_claim_token;

alter table queue_history drop column claim_token;

alter table agent_work_claims drop column policy_digest;

alter table agent_actions drop column verdict;

pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  '''run_submitted'',''run_reconciled'',
    ''review''
  ))',
  '''run_submitted'',''run_reconciled''
  ))'
)
where type = 'table' and name = 'agent_actions';

pragma writable_schema = reset;

delete from queue_schema where version = 2;

delete from schema_migrations where version = 42;
