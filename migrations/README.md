# Migrations

SQL schema migrations for Planar. Authored in plain SQL, applied with
[`sqlx-cli`](https://github.com/launchbadge/sqlx/tree/main/sqlx-cli) at
operational time and embedded into the Zig binary at build time.

## File format

Each migration is a **pair** of files:

```
NNNNN_<name>.up.sql
NNNNN_<name>.down.sql
```

- `NNNNN` — five-digit zero-padded version number, monotonically increasing
  (`00001`, `00002`, …). Lexical sort is also numeric sort.
- `<name>` — short snake_case description of the change.
- `.up.sql` — forward migration.
- `.down.sql` — exact rollback of the corresponding `.up.sql`.

Files are pure SQLite SQL. **No goose-style `-- +goose Up` / `StatementBegin`
markers.** SQLite parses the entire script in one call; semicolons inside
trigger bodies are handled natively.

## `schema_migrations` contract

The first migration creates a `schema_migrations` table:

```sql
create table schema_migrations (
  version     integer primary key,
  applied_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  description text not null
);
```

This is Planar's **public schema-version contract** — independent of
sqlx-cli's own `_sqlx_migrations` bookkeeping table. Every `.up.sql` must
end with:

```sql
insert into schema_migrations (version, description)
values (N, '<short summary>');
```

Every non-foundation `.down.sql` must end with:

```sql
delete from schema_migrations where version = N;
```

The foundation migration's down drops the table entirely, so no explicit
delete is needed there. Read-side tools (in any language) check this table
and refuse to operate against an unsupported schema version.

## Adding a migration

```bash
# From the repo root:
sqlx migrate add -r <name> --source migrations
```

This creates the next-numbered up/down pair. Edit the files, then run the
up/down/up roundtrip test before committing:

```bash
cd src-zig && zig build test
```

The build pipeline auto-discovers new migrations via the codegen step in
`src-zig/build.zig` (`tools/gen_migrations.zig`). No manual manifest
update is required.

## Applying migrations

**Operationally** (against a real SQLite database):

```bash
sqlx migrate run --source migrations --database-url sqlite://./planar.db
```

**Inside the Zig binary** (`src-zig/src/db/migrate.zig`):

```zig
const db = @import("db");
var conn = try db.sqlite.Db.open("planar.db");
defer conn.close();
try db.migrate.applyAll(&conn, allocator);
```

The binary embeds every up and down SQL string at compile time, so it has
no runtime dependency on the `migrations/` directory.

## Tooling

- **Linter** — `.sqlfluff` at the repo root pins dialect to `sqlite` and
  enforces lowercase keywords / 2-space indent. Run `sqlfluff lint
  migrations/` to check.
- **Test** — `src-zig/src/db/migrate.zig` contains the up/down/up roundtrip
  test that runs under `zig build test`.

## Why this layout

- `migrations/` at the repo root (not under `src-zig/`) keeps SQL out of
  the Zig source tree and matches sqlx-cli's default expectation. See the
  rationale captured in this session's design discussion.
- 5-digit zero-padded prefix leaves headroom past 9999 migrations and
  keeps every existing version visually aligned.
- `.up.sql` / `.down.sql` pairs are sqlx-cli's reversible-migration
  convention. Plain SQL means the same files run unchanged under
  `sqlite3 < file.up.sql` for ad-hoc smoke checks.
