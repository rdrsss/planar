-- 00014_audit_log.down.sql
-- Rolls back 00014_audit_log.up.sql


drop index if exists ix_audit_log_actor;
drop index if exists ix_audit_log_scope;
drop index if exists ix_audit_log_recorded_at;
drop index if exists ix_audit_log_entity;
drop table if exists audit_log;

delete from schema_migrations where version = 14;
