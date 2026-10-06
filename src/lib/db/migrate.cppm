/// @file migrate.cppm
/// @brief `planar.db.migrate` — applies the embedded `planar.db.migrations`
/// chain against a `planar.db` connection. Behavior-preserving port (D2)
/// of `zig/src/db/migrate.zig`'s `applyAll`/`rollbackAll`: each pending up
/// migration runs inside its own explicit `lock_mode::immediate`
/// transaction (matching `begin immediate` at zig/src/db/migrate.zig:53 —
/// see `apply_all`'s doc comment for why the lock mode itself is part of
/// the fidelity contract, not just the transaction boundary), and the
/// `schema_migrations` insert/delete contract (migrations/README.md) is
/// honored entirely by the migration SQL itself — the runner never injects
/// those statements.
///
/// Every entry point reads `k_main_version_table`: `planar.db.migrate` serves
/// the one `migrations/` chain, and the agent database's second stream was
/// retired with agent.db (plan 1089), so there is no per-call table choice.

module;

export module planar.db.migrate;

import std;
import planar.db;
import planar.db.migrations;

namespace planar::db {

/// @brief The main database's schema-version table, written by every
/// `migrations/*.up.sql` (migrations/README.md).
export constexpr std::string_view k_main_version_table = "schema_migrations";

/// @brief Reads `schema_migrations`'s highest applied version. A fresh
/// database (the table does not exist yet) reports `0` rather than
/// surfacing the underlying "no such table" failure — mirrors
/// `zig/src/db/migrate.zig`'s `intQuery(...) catch` fallback, which is how
/// `applyAll` knows to start from the beginning of the chain.
/// The table name (`k_main_version_table`) is a compile-time constant spliced
/// into the query text, never operator input.
/// @param conn The connection to query.
/// @return The highest applied version (`0` on a fresh database), or the
/// SQLite failure as a `db_error` for any other kind of failure.
export auto current_version(connection& conn) -> std::expected<std::uint32_t, db_error>;

/// @brief The verdict of the schema-version handshake between a live
/// database and the migration chain a binary embeds.
///
/// ## DECISION RECORD — the schema-version guard (task 6058)
///
/// `schema_migrations` is Planar's public schema-version contract
/// (CLAUDE.md § Migrations, docs/architecture.md § schema contract,
/// migrations/README.md). Five binaries share one SQLite file and each
/// embeds its own copy of the chain, so "which schema is on disk, and does
/// this binary understand it" has to be answered before any binary reads a
/// column. This enum is the single home for that answer; the four
/// `src/cmd/*/context.cpp` `ensure_db()` implementations render it into
/// their own binary's message and exit code rather than each re-deriving
/// the comparison. Four questions were open when this landed, and these
/// are the answers:
///
/// **(1) What counts as incompatible?** `ahead` always. `behind` depends
/// on the binary (see (2)) — a database behind the binary is not corrupt,
/// it is un-migrated, and un-migrated is a repairable state. `gap` is a
/// third case that neither direction covers and that nothing detected
/// before this task: `apply_all` decides what to run from
/// `max(version)` alone, so a database whose applied set has a HOLE (row
/// 5 deleted, rows 1-20 otherwise present) is indistinguishable from a
/// healthy version-20 database. Migration 5 never re-runs, `max` still
/// reports 20, and every binary proceeds believing it has structure that
/// was never created. That is why `gap` outranks `behind` and `current`
/// in the ordering below.
///
/// **CAVEAT, added at task 6695:** that ordering is real but it is NOT
/// what the shipped binaries observe. Every consumer `ensure_db()` reads
/// `compat->live_` -- the raw version -- and runs its own
/// `stored < maximum` / `stored > maximum` comparison BEFORE `verdict_` is
/// consulted, so at the binary level BEHIND outranks GAP, the inverse of
/// the precedence declared here. `verdict_` is read only for the warn-and-
/// proceed `gap` arm. An earlier edition of this comment also justified
/// gap-over-behind as "applying the pending tail on top of a broken base
/// would compound it"; there is no pending tail at that point, because
/// `context.cpp` runs `apply_all` BEFORE `assert_schema_compatible`. Both
/// claims were removed rather than left to mislead a future reader into
/// thinking the ordering is load-bearing where it is not.
///
/// **(2) What does each binary do?** Unchanged by this task and recorded
/// here because it is the reason this is a verdict and not a boolean:
/// `planar` owns migration, so it APPLIES the pending chain and then
/// refuses only `ahead`; `planar-agent`, `planar-watch` and `planar-ext`
/// consume the schema and refuse BOTH directions (`behind` means "run
/// `planar init`"). `planar-execute` holds no SQLite handle and is out of
/// scope entirely. `planar-watch`'s refusal on `behind` was raised as an
/// operator question (task 6691) and DECIDED (decision 1120): keep it. All
/// five binaries ship and install together as one bundle, so a schema
/// mismatch on this binary is an installation-integrity signal, not an
/// ordinary operational state an operator might reasonably run standalone
/// into — refusing loudly is the correct response to "something is wrong
/// with this install," not a footgun. Pinned by
/// `src/cmd/planar-watch/context.t.cpp`.
///
/// **(3) Refuse, or warn?** `ahead` and `behind` refuse: the binary
/// cannot read the schema correctly and continuing would misread columns
/// silently. `gap` WARNS and proceeds. A hard refusal on `gap` would lock
/// the operator out of exactly the diagnostic verbs (`planar health`,
/// `planar-watch ps`) that a corrupt applied-set needs, and unlike a
/// version skew a hole is self-announcing — the first query against the
/// missing structure fails loudly with a SQLite error naming the object.
/// Warning keeps the diagnosis reachable while making the corruption
/// visible; refusing would trade a loud failure for a locked door.
///
/// **(4) Exit code and message?** No new code: `exit_schema_version` (7)
/// already exists in every `src/cmd/*/exit.cppm`, and `gap` does not
/// refuse so it needs none. The per-binary wording is unchanged and stays
/// at each `ensure_db()`, because it is pinned by
/// `src/cmd/*/context.t.cpp` and names that binary's own remediation.
export enum class schema_compatibility : std::uint8_t {
  current, ///< Live version equals the embedded maximum. Proceed.
  behind,  ///< Live version is lower: migrations are pending.
  ahead,   ///< Live version is higher: a newer binary migrated this file.
  gap,     ///< The applied set is not exactly `1..live` — a row is missing.
};

/// @brief The outcome of `assert_schema_compatible`: both versions that
/// were compared, plus the verdict, so a caller can render a message
/// without re-querying.
export struct schema_state {
  std::uint32_t        live_         = 0; ///< `max(version)` in `schema_migrations` (0 on a fresh database).
  std::uint32_t        embedded_max_ = 0; ///< The highest version in the chain the binary embeds.
  schema_compatibility verdict_      = schema_compatibility::current; ///< The handshake verdict.
};

/// @brief The highest version in `chain`.
/// @param chain The migration chain to measure.
/// @return The highest version, or 0 for an empty chain.
export auto embedded_max(std::span<migration_record const> chain) -> std::uint32_t;

/// @brief The highest version in the embedded chain (`migrations()`).
/// @return The highest embedded version.
export auto embedded_max() -> std::uint32_t;

/// @brief Rejects a migration chain whose versions are not exactly
/// `1, 2, ... n` in order.
///
/// Strict monotonicity is NOT enough, and the difference is the whole
/// point: a chain of 1, 2, 5 is strictly increasing, applies cleanly, and
/// leaves `max(version) = 5` on a database where migrations 3 and 4 never
/// ran. Every later handshake then reads "version 5" and believes the
/// database has structure it does not have — the same silent misread the
/// `gap` verdict exists to catch, except originating in the BINARY rather
/// than in the file. Contiguity can only be violated by a build defect
/// (a mis-generated `planar.db.migrations`, a hand-edited chain), never
/// by anything an operator does, so refusing is safe: it cannot lock
/// anyone out of a correctly built binary.
/// @param chain The migration chain to validate.
/// @return Success, or a `db_error` naming the first offending version.
export auto require_contiguous(std::span<migration_record const> chain) -> std::expected<void, db_error>;

/// @brief Compares the live database's schema version against `chain` and
/// reports the verdict. Does not write, does not migrate, and does not
/// decide policy — the caller (each binary's `ensure_db()`) turns the
/// verdict into a refusal, a warning, or a migrate.
///
/// A missing `schema_migrations` table reports live version 0 rather than
/// a failure: a fresh database never touched by `planar init` is a normal
/// state whose answer is "run `planar init`", not a SQLite diagnostic.
/// Every other failure to read the table, including a database this
/// process cannot open or read, IS surfaced, so a caller never reads an
/// access problem as a schema that needs migrating.
/// @param conn The connection to inspect.
/// @param chain The chain this binary embeds.
/// @return The handshake state, or the SQLite failure as a `db_error`.
export auto assert_schema_compatible(connection& conn, std::span<migration_record const> chain)
    -> std::expected<schema_state, db_error>;

/// @brief Convenience overload comparing against the embedded main chain.
/// @param conn The connection to inspect.
/// @return The handshake state, or the SQLite failure as a `db_error`.
export auto assert_schema_compatible(connection& conn) -> std::expected<schema_state, db_error>;

/// @brief Applies every migration in `chain` whose version exceeds the
/// database's current version, in ascending order, each inside its own
/// explicit transaction begun with `lock_mode::immediate`
/// (`connection::begin_transaction(lock_mode::immediate)`). Idempotent:
/// calling this again against an up-to-date database is a no-op.
///
/// The immediate lock mode matters: Planar is a five-binary system
/// sharing one SQLite file, and every binary migrates at startup. Taking
/// the write lock synchronously at `BEGIN` (rather than SQLite's default
/// deferred mode, where no lock is taken until the first statement)
/// means two concurrent starters racing on the same pending migration
/// fail cleanly against each other right at this call — the loser sees
/// `SQLITE_BUSY` from `begin_transaction` before any migration DDL runs
/// — instead of one of them getting `SQLITE_BUSY` partway through the
/// script. This mirrors `begin immediate` at
/// zig/src/db/migrate.zig:53.
/// @param conn The connection to migrate.
/// @param chain The ordered migration chain to apply. Exposed as a
/// parameter — rather than always reading `planar::db::migrations()` — so
/// tests can inject a synthetic chain (e.g. a deliberately broken
/// migration, to exercise the rollback-on-failure path) without touching
/// the real `migrations/` directory.
/// The version table (`schema_migrations`) is read once before the loop to
/// decide where the chain resumes (see `current_version`). The migration SQL,
/// not the runner, writes to it.
/// @return Success, or the first failing migration's `db_error`. That
/// migration's own transaction is rolled back (`transaction`'s
/// destructor rolls back anything not explicitly committed); every
/// migration applied earlier in the same call stays committed.
export auto apply_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error>;

/// @brief Apply `chain` after enforcing that it is CONTIGUOUS from 1.
///
/// The enforcement point `apply_all(connection&)` delegates to (task 6697).
/// It exists as a named, callable function rather than as three inline lines
/// so the enforcement itself can be pinned by a test: the embedded chain is
/// contiguous, so a guard wired only into `apply_all(connection&)` can never
/// fire, and a reviewer probe deleting that call passed the ENTIRE suite
/// (3429 tests, exit 0). The contiguity PREDICATE was pinned;
/// the enforcement was not.
///
/// Do NOT route the span overload through this: that overload is the test
/// seam that deliberately injects partial and synthetic chains, and
/// enforcing contiguity there would reject its whole purpose.
/// @param conn An open database connection.
/// @param chain The migration chain to enforce and apply.
/// @return Success, or the contiguity failure before any migration runs.
export auto apply_contiguous(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error>;

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
