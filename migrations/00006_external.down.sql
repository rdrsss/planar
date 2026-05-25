-- Rolls back 00006_external.up.sql


drop table if exists sync_events;
drop table if exists external_links;
drop table if exists external_systems;

delete from schema_migrations where version = 6;
