-- Rolls back 00009_drop_active_scope.up.sql

create table active_scope (
  position       integer primary key,
  association_id integer not null references associations(id) on delete cascade,
  set_at         text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
create index ix_active_scope_association on active_scope(association_id);

delete from schema_migrations where version = 9;
