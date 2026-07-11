alter table external_links add column baseline_title text;
alter table external_links add column baseline_status text;

insert into schema_migrations (version, description)
values (27, 'external sync per-field baseline for conflict detection');
