-- A competing Planar writer is retryable.  Preserve every existing capture
-- row while widening the durable diagnostic category constraint.
alter table cli_invocations rename to cli_invocations_without_busy;

create table cli_invocations (
  id integer primary key,
  verb_path text not null,
  args_shape text not null default '',
  exit_code integer not null,
  error_category text check (
    error_category in (
      'usage', 'scope', 'not_found', 'conflict',
      'validation', 'io', 'db', 'busy', 'internal'
    )
  ),
  scope_slug text,
  duration_ms integer,
  recorded_at text not null,
  check ((exit_code = 0) = (error_category is null))
);

insert into cli_invocations (id, verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at)
  select id, verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at
    from cli_invocations_without_busy;
drop table cli_invocations_without_busy;

create index idx_cli_invocations_recorded_at on cli_invocations (recorded_at);
create index idx_cli_invocations_verb_path on cli_invocations (verb_path);
create index idx_cli_invocations_exit_code on cli_invocations (exit_code);

insert into schema_migrations (version, description)
values (36, 'cli_invocations: durable retryable busy category');
