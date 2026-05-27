drop index if exists ix_agent_actions_parent;
drop index if exists ix_agent_actions_started;
drop index if exists ix_agent_actions_kind;
drop index if exists ix_agent_actions_active;
drop index if exists ix_agent_actions_entity;
drop index if exists ix_agent_actions_claim;
drop index if exists ix_agent_actions_session;
drop table if exists agent_actions;

drop index if exists ix_agent_work_claims_worktree;
drop index if exists ix_agent_work_claims_heartbeat;
drop index if exists ix_agent_work_claims_vendor;
drop index if exists ix_agent_work_claims_active;
drop index if exists ix_agent_work_claims_entity;
drop index if exists ix_agent_work_claims_session;
drop table if exists agent_work_claims;

delete from schema_migrations where version = 15;
