# Migrations

SQL schema migrations for Planar's main database (`~/.planar/planar.db`).
Authored in plain SQL, applied with
[`sqlx-cli`](https://github.com/launchbadge/sqlx/tree/main/sqlx-cli) at
operational time and embedded into the binary at build time.

This is the only migration stream. The host queue's tables live here too
(migration 00040); the old agent database and its separate stream were
retired in plan 1089, and the installer retires a leftover
`~/.planar/agent.db` (see [INSTALL.md](../INSTALL.md)).

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
make test
```

The build pipeline auto-discovers new migrations via configure-time CMake
codegen (`cmake/generate_migrations.cmake`), which `#embed`s each up/down
pair into the generated `planar.db.migrations` module. Its `file(GLOB
CONFIGURE_DEPENDS)` re-runs configure when the set changes, so no manual
manifest update is required. The codegen takes the module name, namespace
and accessor as parameters and is called once per stream from
`src/lib/db/CMakeLists.txt`; the defaults are this stream's.

## Applying migrations

**Operationally** (against a real SQLite database):

```bash
sqlx migrate run --source migrations --database-url sqlite://./planar.db
```

**Inside the binary** (`src/lib/db/migrate.cppm`) — `planar` owns
migration: it applies every pending migration whenever it opens the
database (`src/cmd/planar/database.cpp`), not only on `planar init`. The
other binaries consume an already-migrated database and deliberately do NOT
call `apply_all` (see `src/cmd/planar-agent/database.cpp` and
`src/cmd/planar-ext/database.cpp`):

```cpp
import planar.db;
import planar.db.migrate;

// The convenience overload applies planar::db::migrations() — the real
// embedded chain. Every fallible boundary returns std::expected.
if (auto applied = db::apply_all(conn); !applied) {
  return std::unexpected(applied.error());
}
```

The binary embeds every up and down SQL string at compile time, so it has
no runtime dependency on the `migrations/` directory.

### Entity-annotation rollback recovery

Migration 00034 deliberately refuses a down migration while entity
annotations or annotation operation receipts exist. Those rows cannot be
represented by the older file-only schema, so rollback never detaches notes
or purges receipts. Export the affected rows (including `annotation_tags`)
from a backup with SQLite before explicitly removing or otherwise disposing
of them through the supported Planar annotation workflow; only then retry the
rollback. Keep the export with the source UUID from `annotation_source_identity`
so it cannot be imported into an unrelated source by numeric ID alone.

### Execution-supervision rollback recovery

Migration 00038 (plan 1033) makes `workflow_runs.plan_id` nullable and widens
`agent_actions.action_kind`. Its down refuses, with
`CHECK constraint failed: m00038_down_refused_null_plan_run_or_supervision_action_kind_present`,
while any run has a null `plan_id` or any action uses one of the five kinds it
added: the older schema cannot represent either, and restoring a constraint by
text edit (below) would not re-check existing rows. Export and dispose of those
rows before retrying; the refusal changes nothing, so a retry is safe.

### Host-queue rollback recovery

Migration 00040 (plan 1089) puts the host-wide build and test queue's
tables, `queue_entries` and `queue_history`, and their compatibility marker
`queue_schema` into `planar.db`, and seeds `queue_entries`' AUTOINCREMENT
counter at 1000000. Its down refuses, with
`CHECK constraint failed: m00040_down_refused_live_queue_entries`, while
`queue_entries` has any row: each row is a command that is waiting or running
now, and dropping the table under its submitter would orphan it. The
refusal changes nothing, so a retry is safe. Roll back in this order:

1. **Drain the queue.** Let the waiting and running commands finish, or
   cancel each one with `planar-agent queue cancel <seq>` (with the binary
   that matches the database). `planar-watch queue` lists what is left.
2. **Stop old submitters.** Make sure no `planar-agent queue run` from the
   build you are leaving is still polling: a submitter started after the
   drain would insert a new row.
3. **Run the down.** With `queue_entries` empty, it drops `queue_schema`,
   `queue_history` and `queue_entries`, which also removes the
   `sqlite_sequence` row, and deletes version 40 from `schema_migrations`.
   **`queue_history` is dropped with it**: the record of every ended run is
   lost. Export it first if you need it.
4. **Archive the logs a reset counter would collide with.** Re-applying the
   migration (or restoring a backup, or re-initializing) starts the counter
   again from the restored `sqlite_sequence` value, or from 1000000 when that
   row is absent. Detached logs are created with `O_EXCL`, so the first
   detached run whose number names an existing `<seq>.log` refuses at exit
   125 rather than overwrite what may be the last record of a run. Run

   ```bash
   python3 scripts/queue-logs-after-reset.py <planar.db>          # list
   python3 scripts/queue-logs-after-reset.py --apply <planar.db>  # archive
   ```

   against the database the counter now lives in. It opens the database
   read-only, takes the threshold from it, and lists the logs in that
   database's log directory (`queue-logs/` beside `planar.db`,
   `<stem>.queue-logs/` for any other file name) numbered above it. With
   `--apply` it moves them into `<log-dir>/reset-archive-<UTC timestamp>/`
   (mode 0700) and prints that path. It never deletes a log, and retention
   never prunes an archive: delete one by hand once you no longer need it.
   It fails closed, changing nothing, on a file it cannot read, a file that
   is not a database, or one with no `sqlite_sequence` table.

Two databases in one directory whose names differ only in their extension
(`x.db` and `x.sqlite`) share `x.queue-logs/`. A log collision between them
is not a counter reset: rename one of the databases instead of running the
helper, which could archive logs the other database's runs are still
writing.

### Relaxing a constraint on a foreign-key parent table

The usual table rebuild is unsafe for a table other tables REFERENCE. The
runtime applies each up migration in a transaction with foreign keys on, where
`foreign_keys` cannot be switched off, so a rebuild either rewrites the
children's REFERENCES clauses to a doomed `_old` table or fires their ON DELETE
actions from DROP TABLE's implicit delete — measured on 00038, it cascade-deleted
`context_records` and nulled claims' `run_id`. For a change that leaves stored
rows valid (removing NOT NULL, widening a CHECK), edit the stored text instead,
as 00038 does: any `ADD COLUMN` / `DROP COLUMN` on that table first, then
`pragma writable_schema = on`, an exact-text `replace()` on `sqlite_schema.sql`,
`pragma writable_schema = reset` (which makes the connection re-read the
schema), and a guard that fails the migration by name if the replacement did
not land.

## Tooling

- **Linter** — `.sqlfluff` at the repo root pins dialect to `sqlite` and
  enforces lowercase keywords / 2-space indent. Run `sqlfluff lint
  migrations/` to check.
- **Test** — `src/lib/db/migrate.t.cpp` contains the up/down/up roundtrip
  test, which runs under `make test` (ctest) against the real embedded
  migrations.

## Why this layout

- `migrations/` at the repo root keeps SQL out of the C++ source tree and
  matches sqlx-cli's default expectation. It sat at the root under the Zig
  implementation for the same reason, and survived the M10 cutover (task
  6045) unchanged — the codegen that consumes it was swapped, not the
  layout.
- 5-digit zero-padded prefix leaves headroom past 9999 migrations and
  keeps every existing version visually aligned.
- `.up.sql` / `.down.sql` pairs are sqlx-cli's reversible-migration
  convention. Plain SQL means the same files run unchanged under
  `sqlite3 < file.up.sql` for ad-hoc smoke checks.
