-- Correct the schema_migrations.description text for versions 2-7.
--
-- Those six rows carried descriptions copy-pasted verbatim from unrelated
-- migrations in the Go archive, so each describes the WRONG migration
-- (e.g. v4 creates `entity_links` but the description said "artifact
-- kinds"). No runtime code reads the `description` column (the version
-- contract is the numeric `version` column; see src/db/migrate.zig, which
-- only queries `max(version)`), so this is a cosmetic correction to the
-- human-readable schema-version record — but it IS the public contract,
-- so it should be accurate. Released migrations are never edited in place;
-- this follow-up migration UPDATEs the rows instead.

update schema_migrations set description = 'planning entities: plans, artifacts, decisions'
where version = 2;
update schema_migrations set description = 'work items: agents, tasks, questions, test_scenarios, plan_steps, active_scope'
where version = 3;
update schema_migrations set description = 'entity_links: typed cross-entity relationships'
where version = 4;
update schema_migrations set description = 'sessions, session_entries, context_snapshots, handoffs'
where version = 5;
update schema_migrations set description = 'external plane: external_systems, external_links, sync_events'
where version = 6;
update schema_migrations set description = 'workbench_sync_state: bidirectional workbench sync tracking'
where version = 7;

insert into schema_migrations (version, description)
values (18, 'correct schema_migrations descriptions for versions 2-7');
