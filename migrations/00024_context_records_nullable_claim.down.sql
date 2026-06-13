-- Down: restore context_records with claim_id NOT NULL.
-- Any capsule rows with claim_id = NULL will be dropped by the NOT NULL constraint;
-- this is acceptable for rollback because those rows were written by the compaction
-- mechanism that does not exist before migration 24.

alter table context_records rename to context_records_new;

create table context_records (
  id            integer primary key autoincrement,
  run_id        integer not null references workflow_runs(id) on delete cascade,
  stage         text not null,
  session_id    integer not null references sessions(id) on delete restrict,
  claim_id      integer not null references agent_work_claims(id) on delete restrict,
  kind          text not null check(kind in (
    'finding','risk','artifact','followup','summary','capsule'
  )),
  body          text not null,
  status        text not null default 'active' check(status in (
    'active','consumed','superseded'
  )),
  compiled_from text,
  created_at    text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

-- Only copy rows that have a non-null claim_id (rows with NULL claim_id
-- cannot be inserted due to the restored NOT NULL constraint).
insert into context_records (id, run_id, stage, session_id, claim_id, kind, body, status, compiled_from, created_at)
select id, run_id, stage, session_id, claim_id, kind, body, status, compiled_from, created_at
from context_records_new
where claim_id is not null;

drop table context_records_new;

create index ix_context_records_run on context_records(run_id);
create index ix_context_records_stage on context_records(run_id, stage);
create index ix_context_records_claim on context_records(claim_id);
create index ix_context_records_session on context_records(session_id);
create index ix_context_records_active on context_records(run_id, stage, status) where status = 'active';
create index ix_context_records_kind on context_records(kind);

delete from schema_migrations where version = 24;
