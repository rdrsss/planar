-- Rolls back 00003_work_items.up.sql


drop table if exists plan_steps;
drop table if exists test_scenarios;
drop table if exists questions;
drop table if exists tasks;
drop table if exists active_scope;
drop table if exists agents;

delete from schema_migrations where version = 3;
