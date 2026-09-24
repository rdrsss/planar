// @file scope_ref.t.cpp
// @brief Unit tests for `planar.scope_ref` (plan 996, task 6089, D19).
// Exercises the extracted `normalize_assoc`/`resolve` primitives directly
// against a real, migrated on-disk SQLite database. The cross-consumer
// agreement test lives in
// src/engine/planning/scope_ref_agreement.t.cpp — it needs both this
// module AND `planar.engine.identity.scope` on the same test binary,
// which is what `TEST_DEPENDS` is for (see that CMakeLists.txt).
//
// Include-before-import is deliberate (see db/db.t.cpp): MSVC's supported
// direction for mixing textual std headers with IFC imports is
// include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.scope_ref;

namespace {

using planar::scope_ref::error;
using planar::scope_ref::normalize_assoc;
using planar::scope_ref::resolve;
using planar::scope_ref::scope_kind;

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Mirrors
/// identity/scope.t.cpp's identical helper — duplicated rather than
/// shared because this codebase has no header tree for first-party code.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_scope_ref_test_{}_{}.db",
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

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto insert_project(planar::db::connection& conn, std::string_view slug, std::string_view root_path) -> std::int64_t {
  auto stmt = conn.prepare("insert into projects (slug, name, root_path) values (?, ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->bind_text(3, root_path).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_association(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into associations (slug, name, kind) values (?, ?, 'org') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

} // namespace

TEST_CASE("normalize_assoc: strips a leading 'assoc:' prefix, leaves other input unchanged", "[scope_ref][normalize_assoc]") {
  CHECK(normalize_assoc("assoc:acme") == "acme");
  CHECK(normalize_assoc("acme") == "acme");
  CHECK(normalize_assoc("repo:acme") == "repo:acme");
  CHECK(normalize_assoc("") == "");
}

TEST_CASE("resolve: 'global' resolves to kind=global with no id", "[scope_ref][resolve]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto ref = resolve(conn, "global");
  REQUIRE(ref.has_value());
  CHECK(ref->kind == scope_kind::global);
  CHECK_FALSE(ref->id.has_value());
}

TEST_CASE("resolve: bare slug and 'assoc:' prefix resolve identically", "[scope_ref][resolve]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_association(conn, "acme");

  auto bare = resolve(conn, "acme");
  REQUIRE(bare.has_value());
  CHECK(bare->kind == scope_kind::association);
  CHECK(bare->id == assoc_id);

  auto prefixed = resolve(conn, "assoc:acme");
  REQUIRE(prefixed.has_value());
  CHECK(prefixed->kind == scope_kind::association);
  CHECK(prefixed->id == assoc_id);
}

TEST_CASE("resolve: 'repo:' prefix resolves against projects", "[scope_ref][resolve]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");

  auto ref = resolve(conn, "repo:myrepo");
  REQUIRE(ref.has_value());
  CHECK(ref->kind == scope_kind::repo);
  CHECK(ref->id == project_id);
}

TEST_CASE("resolve: unknown association/repo slug is slug_not_found", "[scope_ref][resolve]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto assoc_ref = resolve(conn, "no-such-slug");
  REQUIRE_FALSE(assoc_ref.has_value());
  CHECK(assoc_ref.error() == error::slug_not_found);

  auto repo_ref = resolve(conn, "repo:no-such-repo");
  REQUIRE_FALSE(repo_ref.has_value());
  CHECK(repo_ref.error() == error::slug_not_found);
}

TEST_CASE("resolve: an empty slug after stripping a prefix is slug_not_found, not a query", "[scope_ref][resolve]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto bare_empty = resolve(conn, "");
  REQUIRE_FALSE(bare_empty.has_value());
  CHECK(bare_empty.error() == error::slug_not_found);

  auto repo_empty = resolve(conn, "repo:");
  REQUIRE_FALSE(repo_empty.has_value());
  CHECK(repo_empty.error() == error::slug_not_found);

  auto assoc_empty = resolve(conn, "assoc:");
  REQUIRE_FALSE(assoc_empty.has_value());
  CHECK(assoc_empty.error() == error::slug_not_found);
}
