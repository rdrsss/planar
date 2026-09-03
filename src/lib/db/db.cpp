/// @file db.cpp
/// @brief Implementation of `planar.db` (see db.cppm). The vendored SQLite
/// amalgamation's C API is confined to this translation unit's global
/// module fragment — no other file in the module purview names a raw
/// `sqlite3` type.

module;

#include <sqlite3.h>

module planar.db;

import std;

namespace planar::db {

namespace {

/// @brief Monotonic process-local suffix used to make nested transaction
/// SAVEPOINT names unique on a connection, including when a caller supplied
/// its own raw savepoints through `connection::execute`.
std::atomic<std::uint64_t> next_savepoint_id{0};

/// @brief Builds a `db_error` from a connection handle's current extended
/// result code and message.
auto make_error(sqlite3* handle) -> db_error {
  if (handle == nullptr) {
    return db_error{.code_ = SQLITE_ERROR, .message_ = "planar.db: no connection handle"};
  }
  return db_error{.code_ = sqlite3_extended_errcode(handle), .message_ = sqlite3_errmsg(handle)};
}

/// @brief Runs one no-parameter, no-result-row SQL statement (`BEGIN`,
/// `COMMIT`, `ROLLBACK`, `PRAGMA ...`) against `handle`.
auto exec_simple(sqlite3* handle, const char* sql) -> std::expected<void, db_error> {
  char*     errmsg = nullptr;
  const int rc     = sqlite3_exec(handle, sql, nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    db_error err{.code_ = rc, .message_ = errmsg != nullptr ? std::string(errmsg) : std::string(sqlite3_errmsg(handle))};
    if (errmsg != nullptr) {
      sqlite3_free(errmsg);
    }
    return std::unexpected(err);
  }
  return {};
}

} // namespace

// --- statement --------------------------------------------------------------

statement::statement(sqlite3_stmt* handle) noexcept : _handle(handle) {
}

statement::statement(statement&& other) noexcept : _handle(other._handle) {
  other._handle = nullptr;
}

statement& statement::operator=(statement&& other) noexcept {
  if (this != &other) {
    if (_handle != nullptr) {
      sqlite3_finalize(_handle);
    }
    _handle       = other._handle;
    other._handle = nullptr;
  }
  return *this;
}

statement::~statement() {
  if (_handle != nullptr) {
    sqlite3_finalize(_handle);
  }
}

auto statement::bind_null(int index) -> std::expected<void, db_error> {
  const int rc = sqlite3_bind_null(_handle, index);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(sqlite3_db_handle(_handle)));
  }
  return {};
}

auto statement::bind_int64(int index, std::int64_t value) -> std::expected<void, db_error> {
  const int rc = sqlite3_bind_int64(_handle, index, value);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(sqlite3_db_handle(_handle)));
  }
  return {};
}

auto statement::bind_double(int index, double value) -> std::expected<void, db_error> {
  const int rc = sqlite3_bind_double(_handle, index, value);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(sqlite3_db_handle(_handle)));
  }
  return {};
}

auto statement::bind_text(int index, std::string_view value) -> std::expected<void, db_error> {
  // A default-constructed std::string_view has a NULL `data()`, and
  // `sqlite3_bind_text(stmt, i, nullptr, 0, ...)` binds SQL **NULL** rather
  // than the empty string -- silently violating NOT NULL constraints on
  // whatever column the caller was writing. Both null-data and
  // non-null-data empty views are indistinguishable to every caller in this
  // tree (`sv.empty()` is true for both), so the two MUST bind identically.
  // Normalising to a non-null pointer here is the root fix; callers that
  // genuinely want SQL NULL call `bind_null` explicitly.
  char const* bytes = value.data() == nullptr ? "" : value.data();

  // SQLITE_TRANSIENT: SQLite copies the bytes immediately, so `value` does
  // not need to outlive this call.
  const int rc = sqlite3_bind_text(_handle, index, bytes, static_cast<int>(value.size()), SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(sqlite3_db_handle(_handle)));
  }
  return {};
}

auto statement::bind_blob(int index, std::span<const std::byte> value) -> std::expected<void, db_error> {
  const int rc = sqlite3_bind_blob(_handle, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(sqlite3_db_handle(_handle)));
  }
  return {};
}

auto statement::step() -> std::expected<step_result, db_error> {
  const int rc = sqlite3_step(_handle);
  if (rc == SQLITE_ROW) {
    return step_result::row;
  }
  if (rc == SQLITE_DONE) {
    return step_result::done;
  }
  return std::unexpected(make_error(sqlite3_db_handle(_handle)));
}

auto statement::reset() -> std::expected<void, db_error> {
  const int rc = sqlite3_reset(_handle);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(sqlite3_db_handle(_handle)));
  }
  return {};
}

auto statement::is_null(int index) const -> bool {
  return sqlite3_column_type(_handle, index) == SQLITE_NULL;
}

auto statement::column_int64(int index) const -> std::int64_t {
  return sqlite3_column_int64(_handle, index);
}

auto statement::column_double(int index) const -> double {
  return sqlite3_column_double(_handle, index);
}

auto statement::column_text(int index) const -> std::string {
  const auto* ptr = reinterpret_cast<const char*>(sqlite3_column_text(_handle, index));
  const auto  len = static_cast<std::size_t>(sqlite3_column_bytes(_handle, index));
  if (ptr == nullptr) {
    return {};
  }
  return {ptr, len};
}

auto statement::column_blob(int index) const -> std::vector<std::byte> {
  const auto* ptr = reinterpret_cast<const std::byte*>(sqlite3_column_blob(_handle, index));
  const auto  len = static_cast<std::size_t>(sqlite3_column_bytes(_handle, index));
  if (ptr == nullptr || len == 0) {
    return {};
  }
  return {ptr, ptr + len};
}

// --- transaction --------------------------------------------------------------

transaction::transaction(sqlite3* handle, lock_mode mode) noexcept : _handle(handle) {
  if (sqlite3_get_autocommit(_handle) != 0) {
    const char* begin_sql = mode == lock_mode::immediate ? "begin immediate;" : "begin;";
    _active               = static_cast<bool>(exec_simple(_handle, begin_sql));
    return;
  }

  // SQLite forbids a second BEGIN while any transaction/savepoint is active.
  // A SAVEPOINT is its native nested-transaction primitive: release commits
  // only this scope; rollback-to followed by release discards only this scope.
  _savepoint_name = std::format("planar_tx_{}", next_savepoint_id.fetch_add(1, std::memory_order_relaxed));
  _active         = static_cast<bool>(exec_simple(_handle, std::format("savepoint {};", _savepoint_name).c_str()));
  _savepoint      = _active;
}

transaction::transaction(transaction&& other) noexcept
    : _handle(other._handle), _active(other._active), _committed(other._committed), _savepoint(other._savepoint),
      _savepoint_name(std::move(other._savepoint_name)) {
  other._handle    = nullptr;
  other._active    = false;
  other._committed = true;
  other._savepoint = false;
}

transaction& transaction::operator=(transaction&& other) noexcept {
  if (this != &other) {
    rollback_if_active();
    _handle         = other._handle;
    _active         = other._active;
    _committed      = other._committed;
    _savepoint      = other._savepoint;
    _savepoint_name = std::move(other._savepoint_name);

    other._handle    = nullptr;
    other._active    = false;
    other._committed = true;
    other._savepoint = false;
  }
  return *this;
}

transaction::~transaction() {
  rollback_if_active();
}

auto transaction::rollback_if_active() noexcept -> void {
  if (_handle != nullptr && _active && !_committed) {
    // Best-effort: a destructor cannot propagate `std::expected` failure,
    // and rollback failing is itself a signal the connection is already in
    // a bad state.
    if (_savepoint) {
      [[maybe_unused]] auto ignored = exec_simple(
          _handle, std::format("rollback to savepoint {}; release savepoint {};", _savepoint_name, _savepoint_name).c_str());
    } else {
      [[maybe_unused]] auto ignored = exec_simple(_handle, "rollback;");
    }
  }
  _active = false;
}

auto transaction::commit() -> std::expected<void, db_error> {
  if (_handle == nullptr) {
    return std::unexpected(db_error{.code_ = SQLITE_MISUSE, .message_ = "planar.db: commit() on a moved-from transaction"});
  }
  if (_committed) {
    return std::unexpected(db_error{.code_ = SQLITE_MISUSE, .message_ = "planar.db: commit() called twice"});
  }
  if (!_active) {
    return std::unexpected(
        db_error{.code_    = SQLITE_MISUSE,
                 .message_ = "planar.db: commit() on a transaction that was never active (or already rolled back)"});
  }

  auto result = _savepoint ? exec_simple(_handle, std::format("release savepoint {};", _savepoint_name).c_str())
                           : exec_simple(_handle, "commit;");
  if (!result) {
    return std::unexpected(result.error());
  }
  _committed = true;
  _active    = false;
  return {};
}

// --- connection --------------------------------------------------------------

connection::connection(sqlite3* handle, bool read_only) noexcept : _handle(handle), _read_only(read_only) {
}

connection::connection(connection&& other) noexcept
    : _handle(other._handle), _read_only(other._read_only), _write_allowlist(std::move(other._write_allowlist)) {
  other._handle = nullptr;
}

connection& connection::operator=(connection&& other) noexcept {
  if (this != &other) {
    if (_handle != nullptr) {
      sqlite3_close(_handle);
    }
    _handle          = other._handle;
    _read_only       = other._read_only;
    _write_allowlist = std::move(other._write_allowlist);
    other._handle    = nullptr;
  }
  return *this;
}

connection::~connection() {
  if (_handle != nullptr) {
    sqlite3_close(_handle);
  }
}

auto connection::open(std::string_view path) -> std::expected<connection, db_error> {
  const std::string cpath(path);
  sqlite3*          handle = nullptr;
  const int         rc     = sqlite3_open_v2(cpath.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    db_error err = make_error(handle);
    if (handle != nullptr) {
      sqlite3_close(handle);
    }
    return std::unexpected(err);
  }
  sqlite3_extended_result_codes(handle, 1);

  connection conn(handle, false);
  // Explicit per-connection enable, belt-and-suspenders alongside the
  // compile-time SQLITE_DEFAULT_FOREIGN_KEYS=1 default (tech-spec § "db
  // module"; cmake/dependencies.cmake).
  if (auto pragma = conn.execute("pragma foreign_keys = on;"); !pragma) {
    return std::unexpected(pragma.error());
  }
  return conn;
}

auto connection::open_read_only(std::string_view path) -> std::expected<connection, db_error> {
  // Both the `mode=ro` URI parameter and SQLITE_OPEN_READONLY are used
  // together (docs/architecture.md-equivalent rationale ported from
  // zig/src/db/sqlite.zig's openReadOnly doc comment): the flag is
  // authoritative, the URI form is redundant defense-in-depth for
  // planar-watch.
  const std::string uri    = std::format("file:{}?mode=ro", path);
  sqlite3*          handle = nullptr;
  const int         rc     = sqlite3_open_v2(uri.c_str(), &handle, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
  if (rc != SQLITE_OK) {
    db_error err = make_error(handle);
    if (handle != nullptr) {
      sqlite3_close(handle);
    }
    return std::unexpected(err);
  }
  sqlite3_extended_result_codes(handle, 1);

  connection conn(handle, true);
  if (auto pragma = conn.execute("pragma foreign_keys = on;"); !pragma) {
    return std::unexpected(pragma.error());
  }
  return conn;
}

auto connection::is_read_only() const noexcept -> bool {
  return _read_only;
}

auto connection::execute(std::string_view sql) -> std::expected<void, db_error> {
  const std::string csql(sql);
  return exec_simple(_handle, csql.c_str());
}

auto connection::prepare(std::string_view sql) -> std::expected<statement, db_error> {
  sqlite3_stmt* stmt = nullptr;
  const int     rc   = sqlite3_prepare_v2(_handle, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(_handle));
  }
  return statement(stmt);
}

auto connection::begin_transaction(lock_mode mode) -> std::expected<transaction, db_error> {
  transaction txn(_handle, mode);
  if (!txn._active) {
    return std::unexpected(make_error(_handle));
  }
  return txn;
}

namespace {

/// @brief `sqlite3_set_authorizer` callback backing `restrict_writes_to`.
///
/// `user` points at the connection's own `_write_allowlist` (heap-stable
/// for the handle's lifetime). Fires during `sqlite3_prepare_v2`, once per
/// table/column SQLite's compiler touches, with the ACTION code
/// identifying what kind of access it is — this callback only restricts
/// the three write actions; every other action (SELECT, PRAGMA, DDL,
/// transaction control, function calls, ...) is allowed through
/// unconditionally.
auto write_allowlist_authorizer(void* user, int action, const char* arg1, const char*, const char*, const char*) -> int {
  if (action != SQLITE_INSERT && action != SQLITE_UPDATE && action != SQLITE_DELETE) {
    return SQLITE_OK;
  }
  auto const* allowed = static_cast<std::vector<std::string> const*>(user);
  std::string_view const table(arg1 != nullptr ? arg1 : "");
  for (auto const& t : *allowed) {
    if (t == table) {
      return SQLITE_OK;
    }
  }
  return SQLITE_DENY;
}

} // namespace

auto connection::restrict_writes_to(std::span<std::string const> allowed) -> void {
  _write_allowlist = std::make_unique<std::vector<std::string>>(allowed.begin(), allowed.end());
  sqlite3_set_authorizer(_handle, &write_allowlist_authorizer, _write_allowlist.get());
}

} // namespace planar::db
