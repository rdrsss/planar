-- Rename the `blocks` relationship to `depends-on` (planar task 5715).
--
-- The stored name read backwards. Packet assembly gathers a task's
-- dependencies from its OUTGOING edges:
--
--   select to_id from entity_links
--   where from_kind='task' and from_id=? and relationship='blocks'
--
-- and requires each target to be done. So `A --blocks--> B` meant "A depends
-- on B" — the opposite of what the word says. Reading it the natural way
-- produces exactly the wrong edge, and the resulting `invalid_dependency`
-- looks like an unfinished dependency rather than a reversed one.
--
-- The direction of every existing edge is CORRECT under the new name; only
-- the label was wrong. This migration therefore renames in place and does not
-- reverse any edge. Reversing them would corrupt real dependency data.
--
-- SQLite cannot alter a CHECK constraint, so the table is rebuilt. Column
-- order, kinds, unique constraint and indexes are reproduced exactly from
-- migration 00012.
create table entity_links_depends_on (
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
    'derives-from','depends-on','addresses','verifies','cites','supersedes','touches'
  )),
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (from_kind, from_id, to_kind, to_id, relationship)
);

insert into entity_links_depends_on
  (id, from_kind, from_id, to_kind, to_id, relationship, created_at)
select id, from_kind, from_id, to_kind, to_id,
       case when relationship = 'blocks' then 'depends-on' else relationship end,
       created_at
from entity_links;

drop table entity_links;
alter table entity_links_depends_on rename to entity_links;

create index ix_entity_links_from on entity_links(from_kind, from_id);
create index ix_entity_links_to on entity_links(to_kind, to_id);
create index ix_entity_links_relationship on entity_links(relationship);

insert into schema_migrations (version, description)
values (33, 'rename the blocks relationship to depends-on');
