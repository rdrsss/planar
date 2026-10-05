/// @file db.cppm
/// @brief `planar.db` — RAII connection/statement/transaction wrappers over
/// the vendored SQLite amalgamation (tech-spec § Key mechanisms / "db
/// module"). Every fallible boundary surfaces `std::expected<T, db_error>`;
/// no exceptions cross the module boundary. The raw `sqlite3*` /
/// `sqlite3_stmt*` C API types are only ever forward-declared here and used
/// as private implementation state — they never appear in an exported
/// function signature. The full `<sqlite3.h>` include lives in db.cpp's
/// global module fragment.
///
/// Behavior-preserving port of zig/src/db/sqlite.zig (D2): read-write open,
/// strict read-only open (for `planar-watch`), typed prepared-statement
/// bind/column accessors, and scope-exit transaction semantics. The shape
/// is idiomatic C++26 (RAII destructors instead of Zig's explicit
/// `defer`/`finalize`/`close` calls) rather than a line-for-line
/// transliteration.

module;

extern "C" {
struct sqlite3;
struct sqlite3_stmt;
}

export module planar.db;

import std;

namespace planar::db {

/// @brief Error surface for every fallible `planar.db` operation.
///
/// Carries the SQLite (extended) result code and the driver's own error
/// message, so callers can distinguish e.g. `SQLITE_CONSTRAINT_FOREIGNKEY`
/// from a generic failure without re-deriving it themselves.
export struct db_error {
  int         code_ = 0; ///< The SQLite extended result code (e.g. `SQLITE_CONSTRAINT_FOREIGNKEY`).
  std::string message_;  ///< The driver's own error message (`sqlite3_errmsg`).
};

/// @brief True when `err` is a post-timeout `SQLITE_BUSY` (task 6843): the
/// connection could not acquire a competing lock even after its
/// `busy_timeout` window (see `connection::open`) elapsed.
///
/// SQLite reports several EXTENDED busy codes (e.g.
/// `SQLITE_BUSY_SNAPSHOT`), whose low byte is always plain `SQLITE_BUSY`
/// (5) -- the same convention `planar.engine.planning.annotation`'s
/// `command_db_error` already uses. Callers that want to distinguish
/// "retry me" (busy) from "something is actually wrong" (every other
/// failure) should check this instead of comparing `code_` directly.
/// @param err The failure to classify.
/// @return `true` when `err` is a busy-source failure.
export auto is_busy(const db_error& err) noexcept -> bool;

/// @brief True when `err` means this process may not open, read or write the
/// database file or its folder: SQLite's `SQLITE_PERM`, `SQLITE_READONLY`,
/// `SQLITE_IOERR` or `SQLITE_CANTOPEN` (extended codes compared by their low
/// byte, as `is_busy` does). A sandbox that denies writes under the database's
/// folder presents as one of these, so a caller can name the access problem
/// rather than a generic query failure.
/// @param err The failure to classify.
/// @return `true` when `err` is an access failure.
export auto is_access_failure(const db_error& err) noexcept -> bool;

/// @brief The sentence every binary appends when it cannot open, read or
/// write the database: what access Planar needs and the usual cause.
/// @return The sentence, ending in a full stop and no newline.
export auto access_requirement() noexcept -> std::string_view;

/// @brief Outcome of a single `statement::step()` call.
export enum class step_result {
  row, ///< A row is available; read columns before stepping again.
  done ///< The statement has no more rows.
};

/// @brief A prepared SQLite statement.
///
/// Move-only. The underlying `sqlite3_stmt*` is finalized in the
/// destructor. Bind parameters are 1-indexed, matching SQLite's own
/// convention. Never share a `statement` across threads.
export class statement {
private:
  sqlite3_stmt* _handle = nullptr;

  friend class connection;

  /// @brief Takes ownership of an already-prepared statement handle.
  /// @param handle A live, non-null `sqlite3_stmt*` returned by
  /// `sqlite3_prepare_v2`.
  explicit statement(sqlite3_stmt* handle) noexcept;

public:
  statement(const statement&)            = delete;
  statement& operator=(const statement&) = delete;

  /// @brief Transfers ownership of `other`'s prepared statement.
  /// @param other The statement to move from; left with no handle.
  statement(statement&& other) noexcept;
  /// @brief Finalizes this statement's handle (if any), then transfers
  /// ownership of `other`'s.
  /// @param other The statement to move from; left with no handle.
  /// @return `*this`.
  statement& operator=(statement&& other) noexcept;

  /// @brief Finalizes the underlying prepared statement, if still owned.
  ~statement();

  /// @brief Binds SQL NULL at `index` (1-indexed).
  /// @param index The 1-indexed bind parameter position.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto bind_null(int index) -> std::expected<void, db_error>;
  /// @brief Binds a 64-bit integer at `index` (1-indexed).
  /// @param index The 1-indexed bind parameter position.
  /// @param value The integer to bind.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto bind_int64(int index, std::int64_t value) -> std::expected<void, db_error>;
  /// @brief Binds a double-precision float at `index` (1-indexed).
  /// @param index The 1-indexed bind parameter position.
  /// @param value The float to bind.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto bind_double(int index, double value) -> std::expected<void, db_error>;
  /// @brief Binds UTF-8 text at `index` (1-indexed). SQLite copies the
  /// bytes (`SQLITE_TRANSIENT`), so `value` need not outlive the call.
  ///
  /// **Empty binds as `''`, never as SQL NULL.** A default-constructed
  /// `std::string_view` has a null `data()`, which raw
  /// `sqlite3_bind_text` would turn into SQL NULL; this overload
  /// normalises it so a null-data view and an empty-literal view --
  /// which no caller can tell apart, both being `.empty()` -- bind
  /// identically. Callers that want SQL NULL call `bind_null`.
  /// @param index The 1-indexed bind parameter position.
  /// @param value The UTF-8 text to bind.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto bind_text(int index, std::string_view value) -> std::expected<void, db_error>;
  /// @brief Binds a blob at `index` (1-indexed). SQLite copies the bytes
  /// (`SQLITE_TRANSIENT`), so `value` need not outlive the call.
  /// @param index The 1-indexed bind parameter position.
  /// @param value The bytes to bind.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto bind_blob(int index, std::span<const std::byte> value) -> std::expected<void, db_error>;

  /// @brief Advances the statement one step.
  /// @return `step_result::row` when a row is available to read,
  /// `step_result::done` when the statement is exhausted, or the SQLite
  /// failure as a `db_error`.
  auto step() -> std::expected<step_result, db_error>;

  /// @brief Resets the statement so it can be re-executed (bindings are
  /// left intact per SQLite's own `sqlite3_reset` semantics).
  /// @return Success, or the SQLite failure as a `db_error`.
  auto reset() -> std::expected<void, db_error>;

  /// @brief True if the column at `index` (0-indexed) is SQL NULL.
  /// @param index The 0-indexed result column position.
  /// @return `true` if the column is SQL NULL.
  [[nodiscard]] auto is_null(int index) const -> bool;
  /// @brief Reads column `index` (0-indexed) as a 64-bit integer.
  /// @param index The 0-indexed result column position.
  /// @return The column's value.
  [[nodiscard]] auto column_int64(int index) const -> std::int64_t;
  /// @brief Reads column `index` (0-indexed) as a double-precision float.
  /// @param index The 0-indexed result column position.
  /// @return The column's value.
  [[nodiscard]] auto column_double(int index) const -> double;
  /// @brief Reads column `index` (0-indexed) as UTF-8 text.
  /// @param index The 0-indexed result column position.
  /// @return The column's value.
  [[nodiscard]] auto column_text(int index) const -> std::string;
  /// @brief Reads column `index` (0-indexed) as a blob.
  /// @param index The 0-indexed result column position.
  /// @return The column's value.
  [[nodiscard]] auto column_blob(int index) const -> std::vector<std::byte>;
};

/// @brief Requested SQLite lock-acquisition mode for `BEGIN`.
///
/// See `sqlite3`'s own `BEGIN [DEFERRED|IMMEDIATE]` documentation. `deferred`
/// (SQLite's own default) takes no lock at `BEGIN` — the first statement
/// that actually reads or writes acquires it, so a `deferred` transaction
/// that never writes never blocks a concurrent writer. `immediate` acquires
/// the RESERVED (write) lock synchronously at `BEGIN`, before any statement
/// runs, so a concurrent writer (or another `immediate` transaction) on a
/// different connection fails cleanly with `SQLITE_BUSY` right there,
/// rather than partway through whatever script the first transaction goes
/// on to run. See `connection::begin_transaction`.
export enum class lock_mode {
  deferred, ///< No lock until the first read/write statement (SQLite default).
  immediate ///< Takes the write (RESERVED) lock synchronously at `BEGIN`.
};

/// @brief A SQLite transaction, begun immediately on construction.
///
/// Move-only. At SQLite's outermost level it issues `BEGIN` (or `BEGIN
/// IMMEDIATE`); when the same connection already has a transaction in
/// progress it instead owns a uniquely named SAVEPOINT. Thus callers may
/// compose independently-atomic engine operations without accidentally
/// committing or rolling back their enclosing operation. If this transaction
/// has not been explicitly committed by the time it is destroyed, the
/// destructor rolls back precisely its owned scope — "rollback on scope
/// exit" is therefore automatic for every early-return or exception path.
export class transaction {
private:
  sqlite3*    _handle    = nullptr;
  bool        _active    = false;
  bool        _committed = false;
  bool        _savepoint = false;
  std::string _savepoint_name;

  friend class connection;

  /// @brief Issues outermost `BEGIN` (or `BEGIN IMMEDIATE`, per `mode`) or,
  /// when `handle` is already transactional, a uniquely named SAVEPOINT;
  /// then takes ownership of that scope.
  /// `handle` is non-owning — the originating `connection` outlives every
  /// `transaction` it produces.
  /// @param handle The connection to begin a transaction on.
  /// @param mode The lock-acquisition mode to request for an OUTERMOST
  /// transaction. Defaults to `lock_mode::deferred` (SQLite's own default
  /// and this type's historical behavior); pass `lock_mode::immediate` to
  /// take the write lock synchronously at outermost `BEGIN` (see
  /// `lock_mode`). Nested scopes use SQLite SAVEPOINT semantics because a
  /// second `BEGIN` is invalid.
  explicit transaction(sqlite3* handle, lock_mode mode = lock_mode::deferred) noexcept;

  /// @brief Rolls back the in-progress transaction, if any, ignoring the
  /// result (destructors cannot propagate `std::expected` failures).
  auto rollback_if_active() noexcept -> void;

public:
  transaction(const transaction&)            = delete;
  transaction& operator=(const transaction&) = delete;

  /// @brief Transfers ownership of `other`'s in-progress transaction.
  /// @param other The transaction to move from; left inert.
  transaction(transaction&& other) noexcept;
  /// @brief Rolls back this transaction if still active, then transfers
  /// ownership of `other`'s.
  /// @param other The transaction to move from; left inert.
  /// @return `*this`.
  transaction& operator=(transaction&& other) noexcept;

  /// @brief Rolls back the transaction if `commit()` was never called.
  ~transaction();

  /// @brief Commits the transaction. After a successful call the destructor
  /// is a no-op. A nested transaction releases only its owned SAVEPOINT;
  /// its enclosing transaction remains active.
  ///
  /// Guarded against every degenerate call shape (M1 boundary-review
  /// finding R2): calling `commit()` on a moved-from transaction (its
  /// `_handle` is null — see the move constructor/assignment), calling it
  /// a second time on an already-committed transaction, or calling it on
  /// one that is not currently active (e.g. after an explicit rollback)
  /// all return a `db_error{.code_ = SQLITE_MISUSE, ...}` without touching
  /// SQLite — none of them ever reach `sqlite3_exec`. The load-bearing
  /// case is the double-commit: a stray second `commit()` cannot re-issue
  /// `COMMIT` against a connection that has since started an unrelated
  /// transaction (confirmed by break-probe — removing this guard lets a
  /// second `commit()` silently succeed and commit the other transaction).
  /// The moved-from case is defense-in-depth rather than the primary
  /// crash concern the finding raised: this module's vendored SQLite
  /// happens to self-guard a null `sqlite3*` inside `sqlite3_exec`/
  /// `sqlite3_errmsg` (`sqlite3SafetyCheckOk`, checked unconditionally at
  /// those two call sites regardless of `SQLITE_ENABLE_API_ARMOR`) — this
  /// guard exists so the contract does not depend on that SQLite-internal
  /// behavior persisting across a future vendor bump, and so the caller
  /// gets a documented `db_error` instead of an SQLite-internal one.
  /// @return Success, or the SQLite failure (or the `SQLITE_MISUSE`
  /// guard failure above) as a `db_error`.
  auto commit() -> std::expected<void, db_error>;
};

/// @brief A RAII SQLite connection.
///
/// Move-only. Foreign keys are enabled explicitly on every connection this
/// type opens (belt-and-suspenders alongside the compile-time
/// `SQLITE_DEFAULT_FOREIGN_KEYS=1` default — tech-spec § "db module").
///
/// ### Lifetime contract with `statement` (task 6060)
///
/// A `statement` does NOT keep its `connection` alive: it holds only the
/// raw `sqlite3_stmt*`, and the two are independent objects that a caller
/// may legitimately destroy in either order. The destructor and move
/// assignment therefore use `sqlite3_close_v2`, not `sqlite3_close`.
///
/// The distinction is not cosmetic. `sqlite3_close` on a handle with a
/// live prepared statement returns `SQLITE_BUSY` and does NOT free the
/// handle; because the destructor has nowhere to report that, the
/// connection would leak permanently and silently (measured at 158,992
/// bytes for one trivial connection, task 6060). `sqlite3_close_v2`
/// instead marks the connection a zombie and reclaims it when the last
/// statement finalizes, so destruction order stops mattering.
///
/// This deliberately does NOT add an ownership link from `statement` back
/// to `connection`: the cost of that coupling is not justified when the C
/// API already offers the order-independent close.
export class connection {
private:
  sqlite3* _handle    = nullptr;
  bool     _read_only = false;
  /// @brief Heap-stable storage for the write-capability allowlist an
  /// installed authorizer callback closes over via its raw `void*`
  /// userdata pointer. Null when `restrict_writes_to` was never called
  /// (the default, unrestricted, policy every other caller keeps). A
  /// `unique_ptr` rather than an inline `vector` member so a move never
  /// invalidates the address SQLite's authorizer callback was registered
  /// against — only the pointer moves, the pointee's heap address does not.
  std::unique_ptr<std::vector<std::string>> _write_allowlist;

  /// @brief Takes ownership of an already-open handle.
  /// @param handle A live, non-null `sqlite3*` returned by `sqlite3_open_v2`.
  /// @param read_only Whether `handle` was opened via `open_read_only`.
  explicit connection(sqlite3* handle, bool read_only) noexcept;

public:
  connection(const connection&)            = delete;
  connection& operator=(const connection&) = delete;

  /// @brief Transfers ownership of `other`'s open handle.
  /// @param other The connection to move from; left with no handle.
  connection(connection&& other) noexcept;
  /// @brief Closes this connection's handle (if any), then transfers
  /// ownership of `other`'s.
  /// @param other The connection to move from; left with no handle.
  /// @return `*this`.
  connection& operator=(connection&& other) noexcept;

  /// @brief Closes the underlying connection, if still owned.
  ~connection();

  /// @brief Opens `path` read-write, creating the file if it does not
  /// exist (SQLite's own default `sqlite3_open` behavior).
  /// @param path Filesystem path to the database file.
  /// @return The open connection, or the SQLite failure as a `db_error`.
  static auto open(std::string_view path) -> std::expected<connection, db_error>;

  /// @brief Opens an EXISTING database read-write with a bounded lock wait,
  /// touching nothing but what a caller then asks of it.
  ///
  /// Unlike `open`, this never creates the file (a missing one is a
  /// `SQLITE_CANTOPEN` failure even if it vanishes after the caller looked),
  /// sets `busy_timeout` to `busy_timeout_ms` through the C API before any
  /// statement runs, and does not switch the journal mode: setting
  /// `journal_mode = WAL` on a database that already is in WAL is a no-op that
  /// still takes a lock, and on any other it would be a write the caller did not
  /// ask for. Foreign keys are enabled as in `open`.
  /// @param path Filesystem path to an existing database file.
  /// @param busy_timeout_ms How long any statement on this connection may wait
  /// for a competing lock.
  /// @return The open connection, or the SQLite failure as a `db_error`.
  static auto open_existing(std::string_view path, int busy_timeout_ms) -> std::expected<connection, db_error>;

  /// @brief Opens `path` in strict read-only mode for `planar-watch`: both
  /// the `file:<path>?mode=ro` URI form and `SQLITE_OPEN_READONLY` are
  /// used together, so the driver refuses any write SQL
  /// (`SQLITE_READONLY`) regardless of which mechanism a future SQLite
  /// release might relax. Fails if `path` does not already exist. An absolute
  /// `path` is spelled `file://<path>` (empty URI authority), so a path that
  /// begins `//` is never read as a `host`.
  /// @param path Filesystem path to an existing database file.
  /// @return The open read-only connection, or the SQLite failure as a
  /// `db_error`.
  static auto open_read_only(std::string_view path) -> std::expected<connection, db_error>;

  /// @brief True if this connection was opened via `open_read_only`.
  /// @return `true` if this connection is read-only.
  [[nodiscard]] auto is_read_only() const noexcept -> bool;

  /// @brief True when SQLite opened the main database read-only although a
  /// writable open was requested, which it does when the file is not writable
  /// to this process. Also `true` for a connection from `open_read_only`.
  /// @return `true` when writes to the main database will be refused.
  [[nodiscard]] auto is_write_protected() const noexcept -> bool;

  /// @brief The most recent SQLite failure on this connection, as SQLite still
  /// holds it: its extended result code and message. `code_` is 0 when the
  /// last call succeeded. Read it straight after the failing call; any later
  /// call on the connection, a rollback included, replaces it.
  /// @return The last failure, or `{0, ""}`.
  [[nodiscard]] auto last_error() const -> db_error;

  /// @brief True while ANY transaction or savepoint is open on this
  /// connection -- SQLite's own autocommit flag, inverted.
  ///
  /// Exists so a caller can ASSERT that a scope it just finished actually
  /// closed the transaction it opened (task 6787). A leaked `savepoint`
  /// does not fail loudly: SQLite permits re-entering a same-named nested
  /// savepoint, so the next call in a loop appears to work while the
  /// connection quietly stays non-autocommit, and every write after it
  /// hangs off a scope nobody will commit. Without this accessor that
  /// state is unobservable from a test, which is how the leak in
  /// `workbench::sync::pull_to_db` survived every mutation probe.
  ///
  /// Note this reports the CONNECTION's state, not any particular scope's:
  /// it is `true` inside a nested savepoint as well as an outermost
  /// `BEGIN`.
  /// @return `true` when a transaction or savepoint is open.
  [[nodiscard]] auto in_transaction() const noexcept -> bool;

  /// @brief Executes one or more `;`-separated SQL statements with no
  /// bound parameters and no result rows (DDL, scripts).
  /// @param sql The SQL script to execute.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto execute(std::string_view sql) -> std::expected<void, db_error>;

  /// @brief Prepares `sql` for repeated bind/step calls.
  /// @param sql The single SQL statement to prepare.
  /// @return The prepared statement, or the SQLite failure as a `db_error`.
  auto prepare(std::string_view sql) -> std::expected<statement, db_error>;

  /// @brief Begins a transaction on this connection. See `transaction` for
  /// commit/rollback-on-scope-exit semantics. If this connection is already
  /// transactional, returns a nested SAVEPOINT-backed scope rather than
  /// issuing SQLite's invalid second `BEGIN`.
  /// @param mode The lock-acquisition mode to request for an outermost
  /// transaction (see `lock_mode`).
  /// Defaults to `lock_mode::deferred` — pass `lock_mode::immediate` when
  /// the caller needs to serialize against other writers starting at
  /// `BEGIN` itself rather than at the first write statement (e.g.
  /// `planar.db.migrate`'s `apply_all`, which must not let two concurrent
  /// migrators both start executing a migration script).
  /// @return The in-progress transaction, or the SQLite failure as a
  /// `db_error`.
  auto begin_transaction(lock_mode mode = lock_mode::deferred) -> std::expected<transaction, db_error>;

  /// @brief Installs a write-capability authorizer on this connection:
  /// `INSERT`, `UPDATE`, and `DELETE` are permitted only against a table
  /// named in `allowed`; every other write attempt is DENIED by SQLite's
  /// own authorizer (`sqlite3_set_authorizer`) at PREPARE time, before the
  /// statement can execute. `SELECT` and DDL are never restricted by this
  /// call.
  ///
  /// This is deliberately NOT a source-text check: the authorizer callback
  /// fires during SQLite's own statement compilation and is handed the
  /// PARSED table name, so the restriction holds no matter how the SQL was
  /// composed — including a table name interpolated into the query string
  /// at runtime, which a grep over the source cannot see. Planar's own
  /// history has exactly this defect shape (`src/engine/external/
  /// sync.cpp`'s `table_for`-composed `UPDATE`), which is the reason this
  /// method exists rather than a lint over call sites.
  ///
  /// Calling this again replaces the previously installed allowlist.
  /// Passing an empty span denies every `INSERT`/`UPDATE`/`DELETE` on this
  /// connection. Not used by `open`/`open_read_only` themselves — a
  /// connection carries no write restriction until a caller opts in.
  /// @param allowed The exact set of tables this connection may write to.
  auto restrict_writes_to(std::span<std::string const> allowed) -> void;
};

} // namespace planar::db
