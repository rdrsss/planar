-- 00043 drop_routing_tables: the ten routing_* tables leave the schema. Their
-- writers (adaptive routing, dispatch preview and confirm, experiments,
-- packet fact staging) left master; nothing on master and nothing in
-- planar-explorer's schema contract reads them. No other table references
-- one of them.
--
-- The view over routing_candidates goes first, then each table, children
-- before parents so the implicit DELETE of a DROP TABLE never trips a
-- foreign key. A table's indexes and triggers go with it; the named ones
-- are dropped explicitly first, as 00030's down does.
--
-- The installer copies planar.db (with -wal and -shm) into
-- $PLANAR_HOME/backups/ before it applies this migration. The down migration
-- recreates the tables, indexes, triggers and view as they stood after 00032,
-- empty.

drop trigger trg_routing_dispatch_previews_single_use;
drop trigger routing_terminal_samples_immutable_delete;
drop trigger routing_terminal_samples_immutable;
drop trigger routing_terminal_samples_identity;
drop trigger routing_dispatch_events_append_only_delete;
drop trigger routing_dispatch_events_append_only_update;
drop trigger routing_experiments_immutable_delete;
drop trigger routing_experiments_manifest_immutable;
drop trigger routing_host_observations_immutable_delete;
drop trigger routing_host_observations_immutable;
drop trigger routing_dispatch_snapshots_immutable_delete;
drop trigger routing_dispatch_snapshots_immutable;
drop trigger routing_dispatch_snapshots_experiment_identity;

drop view routing_candidate_compatibility_v1;

drop index ix_routing_dispatch_previews_open;
drop index ix_routing_dispatch_previews_task;
drop index ix_routing_terminal_samples_measured;
drop index ix_routing_terminal_samples_cohort;
drop index ix_routing_dispatch_events_fold;
drop index ix_routing_dispatch_snapshots_experiment;
drop index ix_routing_dispatch_snapshots_cohort;
drop index ix_routing_experiments_cohort;
drop index ix_routing_task_facts_source;
drop index ix_routing_task_facts_task;
drop index ix_routing_host_observations_expiry;
drop index ix_routing_host_observations_latest;
drop index ix_routing_candidate_bindings_role_tier;
drop index ix_routing_candidates_fallback;

drop table routing_dispatch_previews;
drop table routing_terminal_samples;
drop table routing_dispatch_events;
drop table routing_dispatch_snapshots;
drop table routing_experiments;
drop table routing_task_facts;
drop table routing_host_observations;
drop table routing_candidate_bindings;
drop table routing_candidates;
drop table routing_legacy_outcomes;

insert into schema_migrations (version, description)
values (43, 'drop the ten routing tables');
