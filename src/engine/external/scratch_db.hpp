// @file scratch_db.hpp
// @brief The scratch-database fixture the three `*.t.cpp` files in this
// bucket share (plan 996, task 6041).
//
// HOME SAFETY IS THE WHOLE REASON THIS EXISTS IN ONE PLACE. Every path below
// is built from `std::filesystem::temp_directory_path()` plus a
// steady-clock-and-address discriminator. Nothing here reads `PLANAR_DB`,
// `PLANAR_HOME` or `HOME`, and nothing here can name
// `~/.planar/planar.db` — which matters more than usual for this bucket,
// because `apply_all` MIGRATES whatever database it is handed, and this
// branch carries a schema the installed binaries do not support.
//
// A private header rather than a module, for the reason json_read.hpp next
// door already states: it is test-only scaffolding shared
// between TUs of the SAME target, so it creates no dependency edge, and
// nothing that ships includes it.
#pragma once

// Included from module-importing test translation units after `import std;`,
// so it deliberately includes no standard header (that exception applies
// only to headers pulled into a global module fragment; `sha256.hpp`, which
// this used to cite as the example, was deleted at task 6407).

#include <catch2/catch_test_macros.hpp>

namespace planar::engine::external::testing {

/// @brief The error of a failed `std::expected`, or unset when it SUCCEEDED.
///
/// Calling `.error()` on an expected that holds a value is undefined
/// behaviour, so `CHECK(call().error() == some_error)` does NOT reliably fail
/// when `call()` unexpectedly succeeds — it reads a dead union member and may
/// compare equal by luck. A break-probe that deleted `update_sync_state`'s
/// not-found check SURVIVED exactly that way. Routing every such assertion
/// through this helper makes an unexpected success compare against
/// `std::nullopt` and fail loudly.
/// @param value The expected to inspect.
/// @return The error, or unset when `value` holds a value.
template <class T, class E> auto err(const std::expected<T, E>& value) -> std::optional<E> {
  if (value.has_value()) {
    return std::nullopt;
  }
  return value.error();
}

/// @brief A temp-directory database path that deletes itself, including its
/// WAL siblings.
struct scratch_db_path {
  std::filesystem::path path_; ///< The scratch file.

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_extsync_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;
  scratch_db_path(scratch_db_path&&)                 = delete;
  scratch_db_path& operator=(scratch_db_path&&)      = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    // The main file's absence is NOT proof the database is gone: under WAL
    // the `-wal` and `-shm` siblings carry committed content of their own.
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

/// @brief Open the scratch path and apply every migration.
/// @param scratch The scratch path.
/// @return The open, migrated connection.
inline auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

/// @brief Insert a task row directly.
///
/// Raw SQL rather than through `engine_planning`, because that bucket is a
/// LAYER-2 PEER this one may not depend on (D18) — the same constraint the
/// production code works under, so the fixture works under it too.
/// @param conn The connection.
/// @param title The task title.
/// @param status The task status.
/// @return The new task id.
inline auto insert_task(planar::db::connection& conn, std::string_view title, std::string_view status) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, title, status) values ('global', ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_text(2, status).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Read one task's title, status and `updated_at`.
/// @param conn The connection.
/// @param id The task id.
/// @return `(title, status, updated_at)`.
inline auto read_task(planar::db::connection& conn, std::int64_t id) -> std::tuple<std::string, std::string, std::string> {
  auto stmt = conn.prepare("select coalesce(title,''), coalesce(status,''), updated_at from tasks where id = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return {stmt->column_text(0), stmt->column_text(1), stmt->column_text(2)};
}

} // namespace planar::engine::external::testing
