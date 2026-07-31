drop trigger if exists trg_routing_dispatch_previews_single_use;
drop index if exists ix_routing_dispatch_previews_open;
drop index if exists ix_routing_dispatch_previews_task;
drop table if exists routing_dispatch_previews;

delete from schema_migrations where version = 31;
