

-- ============================================================
-- Infrastructure
-- ============================================================

-- schema_migrations: public schema-version contract. The Go runtime applies
-- migration files in lexical order and inserts one row per applied migration,
-- recording its version and a short description. Read-side tools (in any
-- language) open the database, query this table, and refuse to operate against
-- an unsupported version per the SQLite-as-the-contract rule in
-- docs/tech-spec.md.
create table schema_migrations (
  version     integer primary key,
  applied_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  description text not null
);

-- config: key-value store for runtime configuration too small to deserve its own
-- table (defaults, feature flags, freshness windows). Never holds credentials.
create table config (
  key        text primary key,
  value      text not null,
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

-- ============================================================
-- Projects (the "repo" — interchangeable terminology in the spec)
-- ============================================================

-- projects: a project (interchangeably "repo") that Planar knows about.
-- The unit that scope_kind='repo' refers to, the host of any workbench export,
-- and the anchor for git-remote-driven auto-detection of associations.
create table projects (
  id         integer primary key autoincrement,
  slug       text not null unique,
  name       text not null,
  root_path  text,
  git_remote text,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_projects_root_path on projects(root_path) where root_path is not null;
create index ix_projects_git_remote on projects(git_remote) where git_remote is not null;

-- ============================================================
-- Scope and association
-- ============================================================

-- associations: many-to-many tag on projects. Frees the user from the assumption
-- of a single workspace root. Kinds include user-defined groupings (org/project/
-- client/personal/ad-hoc) and auto-detected ones (host/path/lang). config_json
-- holds per-association configuration (e.g. github_lead_repo, lead_repo) that
-- does not warrant its own column.
create table associations (
  id            integer primary key autoincrement,
  slug          text not null unique,
  name          text not null,
  kind          text not null check(kind in ('org','project','client','personal','ad-hoc','host','path','lang')),
  auto_detected integer not null default 0 check(auto_detected in (0,1)),
  config_json   text,
  created_at    text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at    text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_associations_kind on associations(kind);

-- project_associations: membership join — which projects belong to which
-- associations. The `source` column distinguishes user-defined memberships from
-- auto-detected ones (git remote, parent path, language ecosystem).
create table project_associations (
  project_id     integer not null references projects(id) on delete cascade,
  association_id integer not null references associations(id) on delete cascade,
  source         text not null check(source in ('auto:git-remote','auto:path','auto:lang','user')),
  created_at     text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  primary key (project_id, association_id)
);

create index ix_project_associations_association on project_associations(association_id);
create index ix_project_associations_source on project_associations(source);

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (1, 'initial schema');
