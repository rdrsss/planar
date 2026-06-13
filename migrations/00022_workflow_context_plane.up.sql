-- workflow_runs: identity and audit record for one planar-execute run. Created
-- and closed by `planar-agent run start/end`; planar-execute itself remains
-- DB-handle-free and shells those verbs. pid + repo_root are stored so crash
-- reconciliation can pid-probe a run and mark it `abandoned` without a terminal
-- verb having been called. status=`abandoned` is reserved for reconciliation
-- only — `run end` never writes it.
create table workflow_runs (
  id            integer primary key autoincrement,
  plan_id       integer not null references plans(id) on delete restrict,
  workflow_name text not null,
  run_identifier text not null,
  pid           integer not null,
  repo_root     text not null,
  started_at    text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  ended_at      text,
  status        text not null default 'running' check(status in (
    'running','completed','failed','interrupted','abandoned'
  ))
);

create index ix_workflow_runs_plan on workflow_runs(plan_id);
create index ix_workflow_runs_status on workflow_runs(status);
create index ix_workflow_runs_pid on workflow_runs(pid) where status = 'running';
create unique index ux_workflow_runs_identifier on workflow_runs(run_identifier);

-- context_records: run-scoped working memory produced and consumed across
-- workflow stages. Distinct from session_entries (narrative timeline) by design
-- (decision 445): context_records are working memory — what the next stage
-- needs — while session_entries are an event log — what happened. Each record
-- carries a kind (finding, risk, artifact, followup, summary, capsule) and a
-- three-value lifecycle status (active → consumed/superseded). Stage close
-- marks raw records consumed/superseded and writes one compiled capsule record
-- whose compiled_from column points back to the raw record ids it distilled
-- (decision 446). claim_id is the worker-side correlation key (decision 447):
-- `context add --claim <token>` stamps run_id, session_id, stage, and task from
-- the claim row server-side; you cannot write those columns directly.
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

create index ix_context_records_run on context_records(run_id);
create index ix_context_records_stage on context_records(run_id, stage);
create index ix_context_records_claim on context_records(claim_id);
create index ix_context_records_session on context_records(session_id);
create index ix_context_records_active on context_records(run_id, stage, status) where status = 'active';
create index ix_context_records_kind on context_records(kind);

insert into schema_migrations (version, description)
values (22, 'workflow context plane: workflow_runs + context_records tables');
