-- Restore the `blocks` spelling. Direction is unchanged in both directions of
-- this migration; only the label moves.
create table entity_links_blocks (
  id           integer primary key autoincrement,
  from_kind    text not null check(from_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision','session','repo','annotation'
  )),
  from_id      integer not null,
  to_kind      text not null check(to_kind in (
    'plan','plan_step','task','question','test_scenario','artifact','decision','session','repo','annotation'
  )),
  to_id        integer not null,
  relationship text not null check(relationship in (
    'derives-from','blocks','addresses','verifies','cites','supersedes','touches'
  )),
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (from_kind, from_id, to_kind, to_id, relationship)
);

insert into entity_links_blocks
  (id, from_kind, from_id, to_kind, to_id, relationship, created_at)
select id, from_kind, from_id, to_kind, to_id,
       case when relationship = 'depends-on' then 'blocks' else relationship end,
       created_at
from entity_links;

drop table entity_links;
alter table entity_links_blocks rename to entity_links;

create index ix_entity_links_from on entity_links(from_kind, from_id);
create index ix_entity_links_to on entity_links(to_kind, to_id);
create index ix_entity_links_relationship on entity_links(relationship);

delete from schema_migrations where version = 33;
