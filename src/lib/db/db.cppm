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

/// @brief A SQLite transaction, begun immediately on construction.
///
/// Move-only. If the transaction has not been explicitly committed by the
/// time it is destroyed, the destructor rolls it back — "rollback on scope
/// exit" is therefore automatic for every early-return or exception path.
export class transaction {
private:
  sqlite3* _handle    = nullptr;
  bool     _active    = false;
  bool     _committed = false;

  friend class connection;

  /// @brief Issues `BEGIN` on `handle` and takes ownership of the
  /// resulting in-progress transaction. `handle` is non-owning — the
  /// originating `connection` outlives every `transaction` it produces.
  /// @param handle The connection to begin a transaction on.
  explicit transaction(sqlite3* handle) noexcept;

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

  /// @brief Commits the transaction. After a successful call the
  /// destructor is a no-op.
  /// @return Success, or the SQLite failure as a `db_error`.
  auto commit() -> std::expected<void, db_error>;
};

/// @brief A RAII SQLite connection.
///
/// Move-only. `close`s the underlying handle in the destructor. Foreign
/// keys are enabled explicitly on every connection this type opens
/// (belt-and-suspenders alongside the compile-time
/// `SQLITE_DEFAULT_FOREIGN_KEYS=1` default — tech-spec § "db module").
export class connection {
private:
  sqlite3* _handle    = nullptr;
  bool     _read_only = false;

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

  /// @brief Opens `path` in strict read-only mode for `planar-watch`: both
  /// the `file:<path>?mode=ro` URI form and `SQLITE_OPEN_READONLY` are
  /// used together, so the driver refuses any write SQL
  /// (`SQLITE_READONLY`) regardless of which mechanism a future SQLite
  /// release might relax. Fails if `path` does not already exist.
  /// @param path Filesystem path to an existing database file.
  /// @return The open read-only connection, or the SQLite failure as a
  /// `db_error`.
  static auto open_read_only(std::string_view path) -> std::expected<connection, db_error>;

  /// @brief True if this connection was opened via `open_read_only`.
  /// @return `true` if this connection is read-only.
  [[nodiscard]] auto is_read_only() const noexcept -> bool;

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
  /// commit/rollback-on-scope-exit semantics.
  /// @return The in-progress transaction, or the SQLite failure as a
  /// `db_error`.
  auto begin_transaction() -> std::expected<transaction, db_error>;
};

} // namespace planar::db
