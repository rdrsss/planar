// @file policy_scope.t.cpp
// @brief Unit tests for `planar.engine.config.policy_scope` (plan 1142): the
// scope-to-association mapping every policy reader shares.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.config.policy_scope;

namespace {

namespace cfg = planar::engine::config;

struct scratch_db {
  std::filesystem::path path_;

  scratch_db()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_policy_scope_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db(const scratch_db&)            = delete;
  scratch_db& operator=(const scratch_db&) = delete;

  ~scratch_db() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto done = conn.execute(sql);
  REQUIRE(done.has_value());
}

} // namespace

TEST_CASE("policy scope resolves an entity's scope to the association whose policy table applies",
          "[config][policy_scope][1142]") {
  scratch_db scratch;
  auto       opened = planar::db::connection::open(scratch.path_.string());
  REQUIRE(opened.has_value());
  auto& conn = *opened;
  REQUIRE(planar::db::apply_all(conn).has_value());

  exec(conn, "insert into associations (id, slug, name, kind) values (1, 'acme', 'Acme', 'org'), (2, 'beta', 'Beta', 'org')");
  exec(conn, "insert into projects (id, slug, name) values (1, 'solo', 'solo'), (2, 'free', 'free'), (3, 'both', 'both')");
  exec(conn, "insert into project_associations (project_id, association_id, source) values "
             "(1, 1, 'user'), (3, 1, 'user'), (3, 2, 'user')");
  exec(conn, "insert into plans (id, scope_kind, scope_id, title, slug) values "
             "(10, 'global', null, 'g', 'g'), (11, 'association', 2, 'a', 'a'), (12, 'repo', 1, 'r1', 'r1'), "
             "(13, 'repo', 2, 'r2', 'r2'), (14, 'repo', 3, 'r3', 'r3')");

  SECTION("a global plan uses the top-level table") {
    auto const got = cfg::policy_association_for_plan(conn, 10);
    REQUIRE(got.has_value());
    CHECK_FALSE(got->slug.has_value());
    CHECK(got->reason == cfg::policy_scope_reason::global_scope);
  }
  SECTION("an association plan names its association") {
    auto const got = cfg::policy_association_for_plan(conn, 11);
    REQUIRE(got.has_value());
    CHECK(got->slug == std::optional<std::string>{"beta"});
    CHECK(got->reason == cfg::policy_scope_reason::association_scope);
  }
  SECTION("a repo plan uses the project's only association") {
    auto const got = cfg::policy_association_for_plan(conn, 12);
    REQUIRE(got.has_value());
    CHECK(got->slug == std::optional<std::string>{"acme"});
    CHECK(got->reason == cfg::policy_scope_reason::project_membership);
  }
  SECTION("a project in no association uses the top-level table") {
    auto const got = cfg::policy_association_for_plan(conn, 13);
    REQUIRE(got.has_value());
    CHECK_FALSE(got->slug.has_value());
    CHECK(got->reason == cfg::policy_scope_reason::project_unassociated);
  }
  SECTION("a project in several associations is ambiguous and uses the top-level table") {
    auto const got = cfg::policy_association_for_plan(conn, 14);
    REQUIRE(got.has_value());
    CHECK_FALSE(got->slug.has_value());
    CHECK(got->reason == cfg::policy_scope_reason::project_ambiguous);
  }
  SECTION("a missing plan, scope row or unknown kind is not_found") {
    CHECK(cfg::policy_association_for_plan(conn, 999).error() == cfg::policy_scope_error::not_found);
    CHECK(cfg::policy_association_for_scope(conn, "association", 999).error() == cfg::policy_scope_error::not_found);
    CHECK(cfg::policy_association_for_scope(conn, "repo", 999).error() == cfg::policy_scope_error::not_found);
    CHECK(cfg::policy_association_for_scope(conn, "bogus", 1).error() == cfg::policy_scope_error::not_found);
    CHECK(cfg::policy_association_for_scope(conn, "repo", std::nullopt).error() == cfg::policy_scope_error::not_found);
  }
  SECTION("reason names are the wire spellings") {
    CHECK(cfg::policy_scope_reason_name(cfg::policy_scope_reason::project_ambiguous) == "project_ambiguous");
    CHECK(cfg::policy_scope_reason_name(cfg::policy_scope_reason::global_scope) == "global_scope");
  }
}
