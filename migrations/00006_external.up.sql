

-- ============================================================
-- External systems
-- ============================================================

-- external_systems: registered Jira / GitHub Issues / GitLab / Linear instances.
-- auth_method points to credentials held outside the database (env var, gh CLI,
-- OS keychain) — planar.db never stores secrets.
create table external_systems (
  id              integer primary key autoincrement,
  kind            text not null check(kind in ('jira','github-issues','gitlab-issues','linear')),
  slug            text not null unique,
  base_url        text,
  default_project text,
  auth_method     text not null check(auth_method in ('token-env','gh-cli','oauth-stored')),
  auth_ref        text not null,
  created_at      text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at      text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_external_systems_kind on external_systems(kind);

-- ============================================================
-- External links
-- ============================================================

-- external_links: Planar entity <-> external ticket mapping. One entity
-- can have multiple links (e.g. a plan mirrored to both a Jira epic and a GitHub
-- milestone). sync_direction governs whether changes flow read-only, write-back,
-- or two-way. config_json caches per-feature propagation state (selected GitHub
-- strategy, project node id, location, probe results) for strategy stickiness.
create table external_links (
  id               integer primary key autoincrement,
  entity_kind      text not null check(entity_kind in ('plan','task','question','test_scenario','artifact','decision','session')),
  entity_id        integer not null,
  system_id        integer not null references external_systems(id) on delete cascade,
  external_id      text not null,
  external_url     text,
  link_role        text not null default 'mirror' check(link_role in ('mirror','parent','child','reference')),
  sync_direction   text not null default 'two-way' check(sync_direction in ('read-only','write-back','two-way')),
  last_synced_at   text,
  last_sync_status text not null default 'never' check(last_sync_status in ('ok','conflict','error','never')),
  config_json      text,
  created_at       text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (entity_kind, entity_id, system_id, external_id)
);

create index ix_external_links_entity on external_links(entity_kind, entity_id);
create index ix_external_links_system on external_links(system_id);
create index ix_external_links_status on external_links(last_sync_status);

-- ============================================================
-- Sync events
-- ============================================================

-- sync_events: append-only log of every pull, push, and workbench conflict event,
-- with outcome and (for conflicts) detail. link_id is nullable — workbench
-- conflict rows have no external_links counterpart. scope distinguishes
-- workbench events ('workbench') from operational-plane events ('external').
-- context_json carries anchor_plan_id, entity_kind, entity_id, file_path,
-- and hash/timestamp fields for workbench conflict rows. Drives
-- `planar audit trail` queries from either direction.
create table sync_events (
  id             integer primary key autoincrement,
  link_id        integer references external_links(id) on delete set null,
  scope          text not null default 'external' check(scope in ('external','workbench')),
  direction      text not null check(direction in ('pull','push')),
  outcome        text not null check(outcome in (
                   'ok','conflict','error','noop',
                   'resolved-fs','resolved-db',
                   'partial','success','failure',
                   'strategy-abandoned','counterpart-missing'
                 )),
  fields_changed text,
  detail         text,
  context_json   text,
  at             text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_sync_events_link on sync_events(link_id) where link_id is not null;
create index ix_sync_events_outcome on sync_events(outcome);
create index ix_sync_events_at on sync_events(at);
create index ix_sync_events_conflicts on sync_events(id) where outcome = 'conflict';
create index ix_sync_events_scope on sync_events(scope);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (6, 'external_links: add config_json for strategy caching (M7.5c Phase B)');
