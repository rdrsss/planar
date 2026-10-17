# Build queue migrations

SQL schema migrations for the build queue database, `build-queue.db`.
The `planar-build-queue` binary owns and migrates it.
It holds the host-wide build and test queue and nothing else; planning state
stays in `planar.db` and its chain under [`migrations/`](../migrations/README.md).

This chain is separate from the main chain. Each database has its own
`schema_migrations` table, and the two chains never run against the same file.

## File format

Same as the main chain: each migration is a pair of files,
`NNNNN_<name>.up.sql` and `NNNNN_<name>.down.sql`, in pure SQLite SQL, embedded
at build time by `cmake/generate_migrations.cmake` into the
`planar.db.migrations` module as `build_queue_migrations()`. Versions are
contiguous from `1`, and a released file is never edited.

## `schema_migrations` contract

Identical to the main chain's contract (migrations/README.md § `schema_migrations`
contract): the chain's first migration creates the table, every `.up.sql` ends
with an insert of its own row, and every non-foundation `.down.sql` deletes its
row. The foundation's down drops the table.

## Compatibility rule (`queue_schema`)

`queue_schema` is the queue's compatibility marker, and its shape and rule are
the same as in `planar.db`:

- The row with the highest `version` is authoritative.
- `compat` is the oldest queue version that may use `queue_entries` and
  `queue_history`. A binary whose queue version is at least `compat` can read
  this database.
- A binary refuses a database whose `compat` is above its own queue version, and
  a database whose `queue_schema` is missing.

Version 1 is `(1, 1)`. A later migration that changes a queue table inserts its
own `queue_schema` row. A change that a binary can ignore keeps `compat` at the
old value.

## Identity row (`queue_identity`)

`queue_identity` names the file. It holds exactly one row, with
`kind = 'planar-build-queue'` and the time the database was created, and the
`check` on `kind` keeps any other value out.

The migration runner reads the table before it applies anything:

- The main chain refuses a database that has a `queue_identity` table.
- The build-queue chain refuses a database that has a `schema_migrations`
  table and no `queue_identity` table. That is a `planar.db`.
- An empty file is accepted by either chain.

A refusal names the two database kinds (a build queue database or a planar.db),
runs no statement, and leaves the file unchanged.
The check lives in `planar.db.migrate` (`apply_all` and `apply_build_queue`), so
both chains get it.

## Applying migrations

The chain is embedded in the binary at build time. It is applied through
`planar::db::apply_build_queue`, which runs the identity check first. The
chain has its own schema version, separate from the main chain's.
