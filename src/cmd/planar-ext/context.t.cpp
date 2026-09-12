// context.t.cpp -- planar-ext's schema-version guard arms (task 6690).
//
// Plain `//`, not `///`: a Doxygen `@file` block makes the doc-comment lint
// document the TU and then demand `@brief` on every TEST_CASE. The other 196
// `.t.cpp` files use plain comments; this matches them (same lesson as
// check.t.cpp, task 6737).
//
// This binary had NO context test file at all: its schema handling was
// exercised only incidentally, by `capability.t.cpp`'s fixture needing a
// migrated database before it could probe the write allowlist. So neither
// the `behind` arm nor the `ahead` arm was pinned here, and task 6058's
// break-probe 2 -- which disabled the ahead comparison -- left this
// binary's suite green.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;

namespace {

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecars) when the guard goes out of scope. Duplicated from
/// `capability.t.cpp` rather than shared, because this codebase has no
/// header tree for first-party code (modules-only, decision D1/D4).
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_ext_context_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

/// @brief Construct a `planar-ext` context pointed at `path`.
/// @param path The database path.
/// @param out The stdout sink.
/// @param err The stderr sink.
/// @return The context.
auto make_context(const std::filesystem::path& path, std::ostream& out, std::ostream& err) -> planar::cmd::ext::context {
  return planar::cmd::ext::context{
      {}, planar::cmd::ext::map_env({{"PLANAR_DB", path.string()}}), std::filesystem::path{}, path, out, err};
}

/// @brief Apply the full embedded migration chain at `path`, the way
/// `planar init` would -- `planar-ext` cannot, which is the point.
/// @param path Where to create it.
auto seed_migrated_db(const std::filesystem::path& path) -> void {
  auto opened = planar::db::connection::open(path.string());
  REQUIRE(opened.has_value());
  REQUIRE(planar::db::apply_all(*opened).has_value());
}

/// @brief The highest version in this binary's embedded chain.
/// @return The maximum embedded migration version.
auto embedded_maximum() -> std::uint32_t {
  std::uint32_t highest = 0;
  for (auto const& record : planar::db::migrations()) {
    highest = std::max(highest, record.version_);
  }
  return highest;
}

} // namespace

TEST_CASE("planar-ext refuses a database migrated past its embedded chain", "[cmd][ext][context]") {
  // Simulated the way `planar`'s equivalent case does: migrate normally, then
  // insert a `schema_migrations` row above the embedded maximum -- exactly
  // the state a newer binary leaves behind.
  scratch_db_path scratch;
  seed_migrated_db(scratch.path_);
  {
    auto opened = planar::db::connection::open(scratch.path_.string());
    REQUIRE(opened.has_value());
    auto const inserted = opened->execute(std::format(
        "insert into schema_migrations (version, description) values ({}, 'from a newer binary');", embedded_maximum() + 1));
    REQUIRE(inserted.has_value());
  }

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx    = make_context(scratch.path_, out, err);
  auto const         opened = ctx.ensure_db();
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::ext::domain_error_kind::schema_version_ahead);
  CHECK(planar::cmd::ext::exit_code(opened.error()) == 7);
}

TEST_CASE("planar-ext refuses a database it would have to migrate", "[cmd][ext][context]") {
  // The `behind` arm. A nonexistent path opens as a brand-new EMPTY database
  // -- schema version 0 -- and this binary has no authority to migrate it.
  scratch_db_path scratch;

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx    = make_context(scratch.path_, out, err);
  auto const         opened = ctx.ensure_db();
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::ext::domain_error_kind::schema_version_behind);
  CHECK(planar::cmd::ext::exit_code(opened.error()) == 7);
}

TEST_CASE("planar-ext accepts a database at exactly its embedded version", "[cmd][ext][context]") {
  // Non-vacuity for the two refusals above: the SAME fixture shape, migrated
  // and not tampered with, opens cleanly. Without this a guard that refused
  // unconditionally would satisfy both cases.
  scratch_db_path scratch;
  seed_migrated_db(scratch.path_);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx    = make_context(scratch.path_, out, err);
  auto const         opened = ctx.ensure_db();
  REQUIRE(opened.has_value());
}
