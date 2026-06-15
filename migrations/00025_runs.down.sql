drop table run_touches;
drop table run_events;
drop table runs;

delete from schema_migrations where version = 25;
