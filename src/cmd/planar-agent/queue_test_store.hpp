// @file queue_test_store.hpp
// @brief Fixture builders for the `planar-agent queue` command tests (plan
// 1089, task qp-agent-queue-tests; test spec 658 § Strategy, "Database
// fixtures are built in scratch files only").
//
// The queue's state lives in `planar.db`, so a test that wants a store builds
// a `planar.db` at the head of the embedded main chain, or one of the
// deliberately wrong shapes the compatibility rules refuse. Every builder
// takes the path and creates the file (and its parent directory) itself.
//
// Include this header AFTER `import std; import planar.db; import
// planar.db.migrate; import planar.db.migrations;`, as parity_harness.hpp is
// included: it must not `#include` a standard library header. It throws
// `std::runtime_error` rather than using Catch2 macros, so it does not depend
// on include order, and a fixture that cannot be built fails the case loudly
// instead of producing a database that quietly passes.
#pragma once

namespace planar::cmd::qfix {

/// @brief The sequence counter's floor: the queue migration seeds
/// `sqlite_sequence` at this value, so the first entry of a fresh `planar.db`
/// is `k_seq_floor + 1`.
inline constexpr std::int64_t k_seq_floor = 1'000'000;

/// @brief The real sequence number of the `n`th entry of a fresh database: a
/// value at or below the floor is an ORDINAL (1 is the first entry) and is
/// moved above it; a value above it is already a real number (one read from a
/// ticket or a row) and passes through. The test helpers apply this to their
/// `seq` parameters so a case can say "the second entry" without spelling the
/// floor.
/// @param n An ordinal or a real sequence number.
inline constexpr auto seq_of(std::int64_t n) -> std::int64_t {
  return n <= k_seq_floor ? k_seq_floor + n : n;
}

/// @brief Fails the calling case when a fixture step did not work.
/// @param step What was being built, for the message.
/// @param ok Whether the step succeeded.
/// @param detail The reason, appended when it did not.
inline void must(std::string_view step, bool ok, std::string_view detail = {}) {
  if (!ok) {
    throw std::runtime_error(std::format("queue fixture: {} failed: {}", step, detail));
  }
}

/// @brief Opens (creating it, and its directory) a `planar.db` at `path` and
/// brings it to the head of the embedded main chain. Idempotent.
/// @param path The database file.
/// @return The open read-write connection, or the SQLite failure.
inline auto open_store(const std::filesystem::path& path) -> std::expected<planar::db::connection, planar::db::db_error> {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  auto opened = planar::db::connection::open(path.string());
  if (!opened) {
    return std::unexpected(opened.error());
  }
  if (auto applied = planar::db::apply_all(*opened); !applied) {
    return std::unexpected(applied.error());
  }
  return opened;
}

/// @brief Runs one statement on a fresh connection to `path`, which must exist.
/// @param path The database file.
/// @param sql The statement.
inline void exec(const std::filesystem::path& path, std::string_view sql) {
  auto opened = planar::db::connection::open_existing(path.string(), 5000);
  must(std::format("open {}", path.string()), opened.has_value(), opened ? "" : opened.error().message_);
  auto ran = opened->execute(sql);
  must(std::string{sql}, ran.has_value(), ran ? "" : ran.error().message_);
}

/// @brief A `planar.db` at head: the compatible, equal-version store.
/// @param path The database file.
inline void head_store(const std::filesystem::path& path) {
  auto opened = open_store(path);
  must(std::format("build {} at head", path.string()), opened.has_value(), opened ? "" : opened.error().message_);
}

/// @brief The head version of the embedded main chain.
inline auto head_version() -> std::uint32_t {
  return planar::db::embedded_max();
}

/// @brief Ahead and compatible: head plus one extra `schema_migrations` row
/// above it, and a queue marker that keeps `compat` at 1 (a newer build's
/// nullable column).
/// @param path The database file.
inline void ahead_store(const std::filesystem::path& path) {
  head_store(path);
  exec(path, "alter table queue_history add column note text");
  exec(path, "insert into queue_schema (version, compat, description) values (2, 1, 'adds queue_history.note')");
  exec(path, std::format("insert into schema_migrations (version, description) values ({}, 'newer')", head_version() + 1));
}

/// @brief Ahead with a queue marker whose `compat` is above this binary's
/// queue version: the queue tables need a newer build.
/// @param path The database file.
inline void incompatible_ahead_store(const std::filesystem::path& path) {
  head_store(path);
  exec(path, "insert into queue_schema (version, compat, description) values (2, 2, 'needs a newer binary')");
  exec(path, std::format("insert into schema_migrations (version, description) values ({}, 'newer')", head_version() + 1));
}

/// @brief Ahead with a queue column renamed and the marker unchanged: shape
/// drift only the column guard can see.
/// @param path The database file.
inline void shape_drift_store(const std::filesystem::path& path) {
  head_store(path);
  exec(path, "alter table queue_entries rename column child_pgid to child_group");
  exec(path, std::format("insert into schema_migrations (version, description) values ({}, 'newer')", head_version() + 1));
}

/// @brief Equal version but no `queue_schema` table: two branches shipped
/// different migrations under the same number.
/// @param path The database file.
inline void foreign_store(const std::filesystem::path& path) {
  head_store(path);
  exec(path, "drop table queue_schema");
}

/// @brief Behind, for refusal tests: head with only its highest
/// `schema_migrations` row deleted, so the version reads head - 1.
/// @param path The database file.
inline void behind_store(const std::filesystem::path& path) {
  head_store(path);
  exec(path, std::format("delete from schema_migrations where version = {}", head_version()));
}

/// @brief The bytes of `path`, for "left exactly as it was" assertions.
/// @param path The file.
/// @return Its contents, empty when absent.
inline auto file_bytes(const std::filesystem::path& path) -> std::string {
  std::ifstream     in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/// @brief The `schema_migrations` versions of the database at `path`,
/// comma-joined in ascending order.
/// @param path The database file.
inline auto applied_versions(const std::filesystem::path& path) -> std::string {
  auto opened = planar::db::connection::open_read_only(path.string());
  must(std::format("open {} read-only", path.string()), opened.has_value(), opened ? "" : opened.error().message_);
  auto stmt = opened->prepare("select version from schema_migrations order by version");
  must("prepare schema_migrations", stmt.has_value(), stmt ? "" : stmt.error().message_);
  std::string out;
  for (;;) {
    auto stepped = stmt->step();
    must("step schema_migrations", stepped.has_value(), stepped ? "" : stepped.error().message_);
    if (*stepped == planar::db::step_result::done) {
      return out;
    }
    out += out.empty() ? "" : ",";
    out += std::to_string(stmt->column_int64(0));
  }
}

/// @brief The number of rows in `queue_entries` plus `queue_history` of the
/// database at `path`.
/// @param path The database file.
inline auto queue_row_count(const std::filesystem::path& path) -> std::int64_t {
  auto opened = planar::db::connection::open_read_only(path.string());
  must(std::format("open {} read-only", path.string()), opened.has_value(), opened ? "" : opened.error().message_);
  auto stmt = opened->prepare("select (select count(*) from queue_entries) + (select count(*) from queue_history)");
  must("prepare count", stmt.has_value(), stmt ? "" : stmt.error().message_);
  auto stepped = stmt->step();
  must("step count", stepped.has_value() && *stepped == planar::db::step_result::row);
  return stmt->column_int64(0);
}


/// @brief Whether the database at `path` was never opened by a connection in
/// this case. A connection to a WAL-mode database leaves a `-wal` and `-shm`
/// beside it, and the seeding connection removed them when it closed, so their
/// absence is the footprint of "never opened". It reads no row: a read-only
/// connection would itself leave the sidecars behind.
/// @param path The database file.
inline auto untouched(const std::filesystem::path& path) -> bool {
  return !std::filesystem::exists(path.string() + "-wal") && !std::filesystem::exists(path.string() + "-shm");
}

} // namespace planar::cmd::qfix
