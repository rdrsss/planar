// @file system.t.cpp
// @brief Tests for `planar.engine.external.system` (plan 996, task 6041).
//
// HOME SAFETY. Every database here comes from `scratch_db.hpp`, whose whole
// purpose is that the path can only ever be under
// `std::filesystem::temp_directory_path()`. Nothing in this file reads
// PLANAR_DB / PLANAR_HOME / HOME.
//
// ORACLE PROVENANCE. Captured by running `./zig/zig-out/bin/planar` against a
// scratch database under a fully pinned environment (`cd <work> && env
// PLANAR_DB=... PLANAR_HOME=... PLANAR_CONFIG_PATH=... HOME=... <binary>` —
// the ordering parity_harness.hpp documents, because `VAR=x cd dir && bin`
// does not export past the `&&`). Two runs are quoted below and every
// assertion in this file is read off one of them.
//
// Run A — `ext register jira sync-jira --base-url http://127.0.0.1:18041
// --project SYNC --auth-env PLANAR_SYNC_TOKEN --json`, then the row as
// sqlite3 printed it:
//
//   1|jira|sync-jira|http://127.0.0.1:18041|SYNC|token-env|PLANAR_SYNC_TOKEN
//
// Run B — `ext register github gh-demo --project acme/demo` (NO --auth-env):
//
//   2|github-issues|gh-demo|https://api.github.com|acme/demo|gh-cli|default
//
// Run B is the load-bearing one. It shows the three things `register_github`
// decides on its own with no operator input: the hard-coded api.github.com
// base URL, the `gh-cli` auth method, and the LITERAL auth_ref `default`.

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.external;

#include "scratch_db.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {

namespace system_ns = planar::engine::external::system;
using planar::engine::external::testing::err;
using planar::engine::external::testing::open_migrated;
using planar::engine::external::testing::scratch_db_path;

} // namespace

TEST_CASE("system-kind stored values are hyphenated for two of the four", "[engine][external][system]") {
  // Same class of trap `sync_direction` carries: `github_issues` is stored as
  // `github-issues`, and emitting the enumerator spelling would violate the
  // CHECK constraint at INSERT time.
  CHECK(system_ns::system_kind_to_text(system_ns::system_kind::jira) == "jira");
  CHECK(system_ns::system_kind_to_text(system_ns::system_kind::github_issues) == "github-issues");
  CHECK(system_ns::system_kind_to_text(system_ns::system_kind::gitlab_issues) == "gitlab-issues");
  CHECK(system_ns::system_kind_to_text(system_ns::system_kind::linear) == "linear");
  CHECK_FALSE(system_ns::system_kind_from_text("github_issues").has_value());
  CHECK_FALSE(system_ns::system_kind_from_text("github").has_value());
  for (auto const kind : {system_ns::system_kind::jira, system_ns::system_kind::github_issues,
                          system_ns::system_kind::gitlab_issues, system_ns::system_kind::linear}) {
    auto const back = system_ns::system_kind_from_text(system_ns::system_kind_to_text(kind));
    REQUIRE(back.has_value());
    CHECK(*back == kind);
  }
}

TEST_CASE("auth-method stored values are hyphenated in all three", "[engine][external][system]") {
  CHECK(system_ns::auth_method_to_text(system_ns::auth_method::token_env) == "token-env");
  CHECK(system_ns::auth_method_to_text(system_ns::auth_method::gh_cli) == "gh-cli");
  CHECK(system_ns::auth_method_to_text(system_ns::auth_method::oauth_stored) == "oauth-stored");
  CHECK_FALSE(system_ns::auth_method_from_text("token_env").has_value());
  for (auto const method :
       {system_ns::auth_method::token_env, system_ns::auth_method::gh_cli, system_ns::auth_method::oauth_stored}) {
    auto const back = system_ns::auth_method_from_text(system_ns::auth_method_to_text(method));
    REQUIRE(back.has_value());
    CHECK(*back == method);
  }
}

TEST_CASE("register_jira stores the captured oracle row", "[engine][external][system]") {
  // Oracle run A, above.
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  auto const stored = system_ns::register_jira(
      conn, {.slug = "sync-jira", .base_url = "http://127.0.0.1:18041", .project = "SYNC", .auth_env = "PLANAR_SYNC_TOKEN"});
  REQUIRE(stored.has_value());
  CHECK(stored->id == 1);
  CHECK(stored->kind == system_ns::system_kind::jira);
  CHECK(stored->slug == "sync-jira");
  CHECK(stored->base_url == std::optional<std::string>{"http://127.0.0.1:18041"});
  CHECK(stored->default_project == std::optional<std::string>{"SYNC"});
  CHECK(stored->auth == system_ns::auth_method::token_env);
  CHECK(stored->auth_ref == "PLANAR_SYNC_TOKEN");
  CHECK_FALSE(stored->created_at.empty());
}

TEST_CASE("register_github without an auth env selects gh-cli auth and ref 'default'", "[engine][external][system]") {
  // Oracle run B, above — the three values the operator never supplied.
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  auto const stored = system_ns::register_github(conn, {.slug = "gh-demo", .project = "acme/demo"});
  REQUIRE(stored.has_value());
  CHECK(stored->kind == system_ns::system_kind::github_issues);
  CHECK(stored->base_url == std::optional<std::string>{"https://api.github.com"});
  CHECK(stored->default_project == std::optional<std::string>{"acme/demo"});
  CHECK(stored->auth == system_ns::auth_method::gh_cli);
  // The literal string `default`, not an empty ref and not the slug.
  CHECK(stored->auth_ref == "default");
}

TEST_CASE("register_github WITH an auth env selects token-env and that variable", "[engine][external][system]") {
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  auto const stored =
      system_ns::register_github(conn, {.slug = "gh-demo", .project = "acme/demo", .auth_env = "PLANAR_TEST_GH_TOKEN"});
  REQUIRE(stored.has_value());
  // The base URL is hard-coded either way — supplying an auth env does not
  // make it configurable.
  CHECK(stored->base_url == std::optional<std::string>{"https://api.github.com"});
  CHECK(stored->auth == system_ns::auth_method::token_env);
  CHECK(stored->auth_ref == "PLANAR_TEST_GH_TOKEN");
}

TEST_CASE("a duplicate slug is slug_exists, not a generic query failure", "[engine][external][system]") {
  // The distinction is what lets the CLI say "already registered" instead of
  // "database error"; collapsing it into query_failed loses that.
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  REQUIRE(system_ns::register_github(conn, {.slug = "dup", .project = "o/r"}).has_value());
  auto const again = system_ns::register_jira(conn, {.slug = "dup", .base_url = "https://x", .project = "P", .auth_env = "E"});
  REQUIRE_FALSE(again.has_value());
  CHECK(err(again) == std::optional{system_ns::system_error::slug_exists});
}

TEST_CASE("show_by_slug and show_by_id agree, and a miss is not_found", "[engine][external][system]") {
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  auto const stored = system_ns::register_github(conn, {.slug = "gh", .project = "o/r"});
  REQUIRE(stored.has_value());

  auto const by_slug = system_ns::show_by_slug(conn, "gh");
  REQUIRE(by_slug.has_value());
  CHECK(by_slug->id == stored->id);
  auto const by_id = system_ns::show_by_id(conn, stored->id);
  REQUIRE(by_id.has_value());
  CHECK(by_id->slug == "gh");

  CHECK(err(system_ns::show_by_slug(conn, "absent")) == std::optional{system_ns::system_error::not_found});
  CHECK(err(system_ns::show_by_id(conn, 9999)) == std::optional{system_ns::system_error::not_found});
}

TEST_CASE("list returns every system ordered by id", "[engine][external][system]") {
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  CHECK(system_ns::list(conn)->empty());
  REQUIRE(
      system_ns::register_jira(conn, {.slug = "b-jira", .base_url = "https://x", .project = "P", .auth_env = "E"}).has_value());
  REQUIRE(system_ns::register_github(conn, {.slug = "a-github", .project = "o/r"}).has_value());

  auto const all = system_ns::list(conn);
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  // Insertion order, NOT slug order — `a-github` was registered second and
  // still comes second.
  CHECK((*all)[0].slug == "b-jira");
  CHECK((*all)[1].slug == "a-github");
}

TEST_CASE("an unparseable enum column reads as query_failed, not a silent default", "[engine][external][system]") {
  // The CHECK constraints make this unreachable through the binary, but the
  // Zig original still maps it to QueryFailed rather than defaulting, and a
  // port that silently defaulted would turn a corrupt row into a plausible
  // one — which is exactly the shape of an undetected schema drift after a
  // future migration.
  //
  // Reaching it needs a table WITHOUT the CHECK, because SQLite polices the
  // constraint on UPDATE as well as INSERT. The table is therefore renamed
  // aside and replaced with a constraint-free stand-in carrying the same
  // column list — deliberately spelled out rather than derived, so that a
  // migration adding a column makes this probe FAIL LOUDLY (the module's
  // SELECT would find a missing column) instead of quietly stopping to
  // discriminate.
  scratch_db_path const scratch;
  auto                  conn = open_migrated(scratch);

  REQUIRE(conn.execute("alter table external_systems rename to external_systems_real").has_value());
  REQUIRE(conn.execute("create table external_systems ("
                       "  id integer primary key,"
                       "  kind text not null,"
                       "  slug text not null,"
                       "  base_url text,"
                       "  default_project text,"
                       "  auth_method text not null,"
                       "  auth_ref text not null,"
                       "  created_at text not null default '2026-01-01T00:00:00.000Z',"
                       "  updated_at text not null default '2026-01-01T00:00:00.000Z')")
              .has_value());
  REQUIRE(conn.execute("insert into external_systems (kind, slug, auth_method, auth_ref) "
                       "values ('mystery', 'gh', 'token-env', 'E')")
              .has_value());

  auto const bad_kind = system_ns::show_by_slug(conn, "gh");
  REQUIRE_FALSE(bad_kind.has_value());
  CHECK(err(bad_kind) == std::optional{system_ns::system_error::query_failed});

  // The same guard on the OTHER enum column — two separate re-parses, so one
  // of them passing says nothing about the other.
  REQUIRE(conn.execute("update external_systems set kind = 'jira', auth_method = 'psychic'").has_value());
  auto const bad_auth = system_ns::show_by_slug(conn, "gh");
  REQUIRE_FALSE(bad_auth.has_value());
  CHECK(err(bad_auth) == std::optional{system_ns::system_error::query_failed});

  // `list` re-parses through the same helper and must refuse identically
  // rather than skipping the row.
  CHECK(err(system_ns::list(conn)) == std::optional{system_ns::system_error::query_failed});
}
