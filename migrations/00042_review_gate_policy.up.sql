-- 00042 review_gate_policy — plan 1142, task mo-schema-review-gate (tech spec
-- § Schema delta). Three changes, and nothing else:
--
--   agent_actions      action_kind gains 'review'
--                      + verdict ('approve' | 'request-changes', or null)
--   agent_work_claims  + policy_digest (the claim policy the claim was taken
--                      under, or null)
--   queue_history      + claim_token (the claim a queued run named, or null),
--                      indexed; queue_schema gains the row (2, 1)
--
-- Every pre-existing row keeps its meaning: an existing action reads verdict
-- null, an existing claim reads policy_digest null, and an existing history
-- row reads claim_token null, which is what a run without `--claim` records.
--
-- verdict is checked on its values only. It is not tied to action_kind by a
-- cross-column CHECK: the engine that records a review owns that pairing, and
-- a cross-column rule on this foreign-key parent could only be relaxed later
-- by another stored-text edit.
--
-- ## Why action_kind is widened by editing sqlite_schema, not by rebuild
--
-- agent_actions is a foreign-key parent (parent_action_id references it), and
-- the runtime applies this file inside a transaction with foreign keys ON, so
-- a table rebuild would fire ON DELETE actions or rewrite the REFERENCES
-- clause (migrations/README.md § Relaxing a constraint on a foreign-key parent
-- table). Widening a CHECK leaves every stored row valid, so this file uses
-- 00038's technique: ADD COLUMN first (it splices into the stored text at an
-- offset remembered from the connection's last parse, so it must not follow
-- an edit it has not seen), then an exact-text replace() of 00038's list
-- under writable_schema, then `pragma writable_schema = reset`, then a guard
-- that fails the migration by name if the replacement did not land.
--
-- ## The queue marker
--
-- claim_token is a nullable column that keeps the old meaning when null, so
-- compat stays 1: a planar-agent at queue version 1 still passes the queue
-- check against this database (its column guard names none of the new
-- columns), and one at queue version 2 needs this row.

-- agent_actions: verdict is added BEFORE the text edit below.
alter table agent_actions
add column verdict text check (verdict in ('approve', 'request-changes'));

-- agent_work_claims: an added column suffices.
alter table agent_work_claims add column policy_digest text;

-- queue_history: an added column and its index suffice.
alter table queue_history add column claim_token text;

create index idx_queue_history_claim_token on queue_history (claim_token);

pragma writable_schema = on;

update sqlite_schema
set sql = replace(
  sql,
  '''run_submitted'',''run_reconciled''
  ))',
  '''run_submitted'',''run_reconciled'',
    ''review''
  ))'
)
where type = 'table' and name = 'agent_actions';

-- Clears the flag AND makes this connection re-read sqlite_schema, so the
-- widened constraint holds for every statement after this one.
pragma writable_schema = reset;

-- Fail by name if the edit did not land. TEMP, so it never touches the
-- database file; dropped on success, rolled back with everything else on
-- failure.
create temp table m00042_up_guard (
  refusal text
    constraint m00042_up_refused_unexpected_stored_schema_text_action_kind_not_widened
    check (refusal is null)
);
insert into temp.m00042_up_guard (refusal)
select 'unwidened'
where not exists (
  select 1 from sqlite_schema
  where type = 'table' and name = 'agent_actions' and sql like '%''run_reconciled'',
    ''review''
  ))%'
);
drop table temp.m00042_up_guard;

insert into queue_schema (version, compat, description)
values (2, 1, 'queue_history.claim_token: the claim a queued run named');

insert into schema_migrations (version, description)
values (42, 'review gate: review action kind and verdict, claim policy digest, queue history claim token');
