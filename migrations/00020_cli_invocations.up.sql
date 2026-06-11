-- cli_invocations: opt-in local log of operator CLI usage. One row per
-- top-level `planar` invocation when [introspection].cli_log is enabled.
-- Privacy contract: args_shape carries flag names and positional arity
-- only — argument/flag VALUES are never written to this table. Rows older
-- than [introspection].retention_days are pruned statelessly on the
-- capture path.
create table cli_invocations (
  id integer primary key,
  verb_path text not null,             -- e.g. 'task add', 'workbench push'
  args_shape text not null default '', -- e.g. '<pos:1> --plan --json'
  exit_code integer not null,
  error_category text check (
    error_category in (
      'usage', 'scope', 'not_found', 'conflict',
      'validation', 'io', 'db', 'internal'
    )
  ),
  scope_slug text,                     -- resolved scope, when derivable
  duration_ms integer,
  recorded_at text not null,
  -- error_category is present exactly when the invocation failed
  check ((exit_code = 0) = (error_category is null))
);

create index idx_cli_invocations_recorded_at
  on cli_invocations (recorded_at);
create index idx_cli_invocations_verb_path
  on cli_invocations (verb_path);
create index idx_cli_invocations_exit_code
  on cli_invocations (exit_code);

insert into schema_migrations (version, description)
values (20, 'opt-in cli_invocations usage log');
