create table feedback_triage (
  id integer primary key autoincrement,
  finding_task_id integer references tasks(id) on delete cascade,
  finding_question_id integer references questions(id) on delete cascade,
  severity text not null check(severity in ('info','low','medium','high','critical')),
  disposition text not null check(disposition in ('untriaged','needs-reproduction','accepted','retained-question','dismissed','reported-external','duplicate')),
  reproduction_status text not null check(reproduction_status in ('not-run','reproduced','not-reproduced','inconclusive')),
  duplicate_of_triage_id integer references feedback_triage(id) on delete set null,
  evidence_summary text,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check ((finding_task_id is null) != (finding_question_id is null)),
  check ((disposition = 'duplicate') = (duplicate_of_triage_id is not null)),
  check (duplicate_of_triage_id is null or duplicate_of_triage_id != id)
);

create unique index ux_feedback_triage_task
on feedback_triage(finding_task_id) where finding_task_id is not null;

create unique index ux_feedback_triage_question
on feedback_triage(finding_question_id) where finding_question_id is not null;

create index ix_feedback_triage_filters
on feedback_triage(severity, disposition, updated_at, id);

insert into schema_migrations (version, description)
values (28, 'structured feedback triage for task and question findings');
