-- Rolls back 00004_entity_links.up.sql


drop table if exists entity_links;

delete from schema_migrations where version = 4;
