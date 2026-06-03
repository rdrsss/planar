-- Restore the original (incorrect) descriptions so the down migration is a
-- faithful inverse of the up. These are the verbatim strings the v2-v7
-- rows carried before 00018.

update schema_migrations set description = 'planning pipeline: workbench sync, plan slugs, cross-repo links, association config'
where version = 2;
update schema_migrations set description = 'workbench conflict persistence: nullable link_id, scope column, context_json'
where version = 3;
update schema_migrations set description = 'artifact kinds: add product_spec and roadmap'
where version = 4;
update schema_migrations set description = 'session_entries: add read prefix for read-only verbs'
where version = 5;
update schema_migrations set description = 'external_links: add config_json for strategy caching (M7.5c Phase B)'
where version = 6;
update schema_migrations set description = 'sync_events: extend outcome CHECK for strategy-abandoned + counterpart-missing'
where version = 7;

delete from schema_migrations
where version = 18;
