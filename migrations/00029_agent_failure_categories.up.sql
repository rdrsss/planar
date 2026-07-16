alter table agent_work_claims
add column failure_category text check (failure_category in (
  'usage_limit',
  'context_limit',
  'output_limit',
  'tool_failure',
  'validation',
  'unknown'
));

create index ix_agent_work_claims_failure_category
on agent_work_claims (failure_category, claimed_at)
where failure_category is not null;

insert into schema_migrations (version, description)
values (29, 'agent claim terminal failure categories');
