# Agent database migrations

SQL schema migrations for the agent database (`~/.planar/agent.db`, plan
1080, decision 1181). This is a second, independent stream: the same
sqlx-cli file format and authoring rules as [`migrations/`](../migrations/README.md),
a different database file, a different version table, and a different
generated module.

## What differs from `migrations/`

| | `migrations/` | `migrations-agent/` |
|---|---|---|
| Database | `~/.planar/planar.db` (`PLANAR_DB`) | `~/.planar/agent.db` (`PLANAR_AGENT_DB`) |
| Version table | `schema_migrations(version, applied_at, description)` | `agent_schema_migrations(version, compat, description)` |
| Generated module | `planar.db.migrations`, `planar::db::migrations()` | `planar.db.migrations_agent`, `planar::db::agent::migrations()` |
| Applied by | `planar::db::apply_all(conn)` | `planar::db::apply_contiguous(conn, agent::migrations(), k_agent_version_table)` |

Both streams are embedded by the same configure-time codegen
(`cmake/generate_migrations.cmake`, called once per stream from
`src/lib/db/CMakeLists.txt` with the stream's directory, module name,
namespace and accessor) and applied by the same runner
(`src/lib/db/migrate.cppm`, which takes the version table as a parameter).
Neither stream knows about the other: a migration here never names a main
database table, and the codegen for one stream never reads the other's
directory.

## `agent_schema_migrations` contract

The first migration creates:

```sql
create table agent_schema_migrations (
  version     integer primary key,
  compat      integer not null,
  description text not null
);
```

Every `.up.sql` must end with:

```sql
insert into agent_schema_migrations (version, compat, description)
values (N, <compat>, '<short summary>');
```

and every non-foundation `.down.sql` must end with:

```sql
delete from agent_schema_migrations where version = N;
```

The foundation migration's down drops the table entirely.

`compat` is the oldest binary schema version that may open this store. The
row with the highest `version` is authoritative. A migration that only adds
tables, columns with defaults, or indexes keeps the previous migration's
`compat`, so a `planar-agent` from an older build keeps working while a
newer one upgrades the store under it. A migration that drops, renames, or
changes the meaning of anything sets `compat` to its own version. So for
every migration `N`, `compat` is either the previous migration's `compat`
or `N` itself, never anything else.

A binary's own agent schema version is the head of the chain it embeds.
`planar::db::agent::open_agent_db` (`src/lib/db/agentdb.cppm`) runs
`check_compat` after opening the file and before applying any migration:
it reads the highest row and refuses the store, leaving the file unchanged,
only when that row's `compat` is higher than the binary's version. A store
that is ahead of the binary but still compatible is opened as it is.

## The pinned compat table

`src/lib/db/agentdb.t.cpp` (`every agent migration's compat value is
pinned`) lists every `.up.sql` here with the `compat` it must insert, and
checks the value the migration actually writes by applying the chain one
migration at a time to a scratch store. The test fails, naming the file,
when a migration's inserted `compat` differs from the table, when a
migration is missing from the table, or when the table names a file that
does not exist.

Adding a migration therefore always adds one row to that table, with the
value the rule above dictates. Changing an existing value is a reviewed
edit of the table, made in the same change as the migration that needs it,
and the review question is whether the migration really drops, renames or
changes meaning.

## Adding a migration

```bash
# From the repo root:
sqlx migrate add -r <name> --source migrations-agent
```

Then edit the pair, keep the rules above (a `-- <table>: ...` comment above
every `create table`, indexes directly after their table, CHECK constraints
on enum-shaped columns, no `BEGIN`, `COMMIT` or `PRAGMA`), and run
`make test`. Configure re-runs automatically when a file is added or removed
here.
