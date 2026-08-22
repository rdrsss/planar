/// @file migrate.cppm
/// @brief `planar.db.migrate` — applies the embedded `planar.db.migrations`
/// chain against a `planar.db` connection. Behavior-preserving port (D2)
/// of `zig/src/db/migrate.zig`'s `applyAll`/`rollbackAll`: each pending up
/// migration runs inside its own explicit transaction, and the
/// `schema_migrations` insert/delete contract (migrations/README.md) is
/// honored entirely by the migration SQL itself — the runner never injects
/// those statements.

module;

export module planar.db.migrate;

import std;
import planar.db;
import planar.db.migrations;

namespace planar::db {

/// @brief Reads `schema_migrations`'s highest applied version. A fresh
/// database (the table does not exist yet) reports `0` rather than
/// surfacing the underlying "no such table" failure — mirrors
/// `zig/src/db/migrate.zig`'s `intQuery(...) catch` fallback, which is how
/// `applyAll` knows to start from the beginning of the chain.
/// @param conn The connection to query.
/// @return The highest applied version (`0` on a fresh database), or the
/// SQLite failure as a `db_error` for any other kind of failure.
export auto current_version(connection& conn) -> std::expected<std::uint32_t, db_error>;

/// @brief Applies every migration in `chain` whose version exceeds the
/// database's current version, in ascending order, each inside its own
/// explicit transaction (`connection::begin_transaction`). Idempotent:
/// calling this again against an up-to-date database is a no-op.
/// @param conn The connection to migrate.
/// @param chain The ordered migration chain to apply. Exposed as a
/// parameter — rather than always reading `planar::db::migrations()` — so
/// tests can inject a synthetic chain (e.g. a deliberately broken
/// migration, to exercise the rollback-on-failure path) without touching
/// the real `migrations/` directory.
/// @return Success, or the first failing migration's `db_error`. That
/// migration's own transaction is rolled back (`transaction`'s
/// destructor rolls back anything not explicitly committed); every
/// migration applied earlier in the same call stays committed.
export auto apply_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error>;

/// @brief Convenience overload applying `planar::db::migrations()` — the
/// real embedded chain.
/// @param conn The connection to migrate.
/// @return Success, or the failing migration's `db_error`.
export auto apply_all(connection& conn) -> std::expected<void, db_error>;

/// @brief Runs every down migration in `chain`, in descending version
/// order, unconditionally — mirrors `zig/src/db/migrate.zig`'s
/// `rollbackAll` (no per-version check, no per-migration transaction).
/// Each down migration is itself responsible for deleting its own
/// `schema_migrations` row (migrations/README.md contract); the
/// foundation migration's down drops the table entirely.
/// @param conn The connection to roll back.
/// @param chain The ordered migration chain to roll back.
/// @return Success, or the first failure as a `db_error`.
export auto rollback_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error>;

/// @brief Convenience overload rolling back `planar::db::migrations()`.
/// @param conn The connection to roll back.
/// @return Success, or the first failure as a `db_error`.
export auto rollback_all(connection& conn) -> std::expected<void, db_error>;

} // namespace planar::db
