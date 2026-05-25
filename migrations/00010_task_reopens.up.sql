

-- task_reopens: every reopen of a terminal task (done → todo/doing or
-- cancelled → todo/doing). Source captures which CLI surface produced the
-- entry so future audit-trail rendering can distinguish the dedicated verb
-- from the universal escape hatch.
create table task_reopens (
  id           integer primary key autoincrement,
  task_id      integer not null references tasks(id) on delete cascade,
  from_status  text not null check(from_status in ('done','cancelled')),
  to_status    text not null check(to_status in ('todo','doing','blocked')),
  source       text not null check(source in ('task-reopen','task-update-force')),
  reason       text,
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_task_reopens_task on task_reopens(task_id);
create index ix_task_reopens_created on task_reopens(created_at);

insert into schema_migrations (version, description)
values (10, 'task_reopens audit table — done/cancelled escape hatch (plan 215 M1)');
