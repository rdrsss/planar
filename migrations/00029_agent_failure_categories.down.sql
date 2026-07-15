drop index ix_agent_work_claims_failure_category;

alter table agent_work_claims drop column failure_category;

delete from schema_migrations
where version = 29;
