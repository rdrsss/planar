

-- ============================================================
-- Entity links
-- ============================================================

-- entity_links: generic cross-cutting relationship table. Any entity kind can
-- reference any other with a typed relationship (derives-from, blocks, addresses,
-- verifies, cites, supersedes, touches). Also serves as the session<->artifact
-- link surface — there are no per-pair join tables. from_kind and to_kind include
-- 'repo' for cross-repo feature relationships.
create table entity_links (
  id           integer primary key autoincrement,
  from_kind    text not null check(from_kind in ('plan','plan_step','task','question','test_scenario','artifact','decision','session','repo')),
  from_id      integer not null,
  to_kind      text not null check(to_kind in ('plan','plan_step','task','question','test_scenario','artifact','decision','session','repo')),
  to_id        integer not null,
  relationship text not null check(relationship in ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')),
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (from_kind, from_id, to_kind, to_id, relationship)
);

create index ix_entity_links_from on entity_links(from_kind, from_id);
create index ix_entity_links_to on entity_links(to_kind, to_id);
create index ix_entity_links_relationship on entity_links(relationship);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (4, 'artifact kinds: add product_spec and roadmap');
