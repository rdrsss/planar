-- No busy rows can be represented by the prior constraint.  Refuse a
-- destructive downgrade if any have been recorded; all other captures keep
-- their original ids and values across the reversible table rebuild.
select case when exists (select 1 from cli_invocations where error_category = 'busy')
  then abs(-9223372036854775808) else 0 end;

alter table cli_invocations rename to cli_invocations_with_busy;

create table cli_invocations (
  id integer primary key,
  verb_path text not null,
  args_shape text not null default '',
  exit_code integer not null,
  error_category text check (
    error_category in (
      'usage', 'scope', 'not_found', 'conflict',
      'validation', 'io', 'db', 'internal'
    )
  ),
  scope_slug text,
  duration_ms integer,
  recorded_at text not null,
  check ((exit_code = 0) = (error_category is null))
);

insert into cli_invocations (id, verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at)
  select id, verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at
    from cli_invocations_with_busy;
drop table cli_invocations_with_busy;

create index idx_cli_invocations_recorded_at on cli_invocations (recorded_at);
create index idx_cli_invocations_verb_path on cli_invocations (verb_path);
create index idx_cli_invocations_exit_code on cli_invocations (exit_code);

delete from schema_migrations where version = 36;
