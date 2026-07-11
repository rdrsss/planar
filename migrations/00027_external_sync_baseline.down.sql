delete from schema_migrations where version = 27;

alter table external_links drop column baseline_status;
alter table external_links drop column baseline_title;
