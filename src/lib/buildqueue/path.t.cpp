// @file path.t.cpp
// @brief Unit tests for `planar.buildqueue.path` (plan 1163, decision 1438).
// Every case drives the resolver with an injected map, so no test reads or
// sets a process environment variable.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

import std;
import planar.buildqueue.path;

namespace {

using planar::buildqueue::env_lookup;
using planar::buildqueue::error;
using planar::buildqueue::k_env_db;
using planar::buildqueue::k_env_slot;

/// @brief A lookup over a fixed map of variables.
/// @param vars The variables the lookup knows; every other name is absent.
/// @return An environment lookup over `vars`.
auto env_of(std::map<std::string, std::string, std::less<>> vars) -> env_lookup {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    if (auto const found = vars.find(name); found != vars.end()) {
      return found->second;
    }
    return std::nullopt;
  };
}

} // namespace

TEST_CASE("path: PLANAR_BUILD_QUEUE_DB wins, and its log directory sits beside it", "[buildqueue][path]") {
  auto const env = env_of({{std::string{k_env_db}, "/x/q.db"}, {"HOME", "/h"}});

  auto const db = planar::buildqueue::resolve_queue_db_path(env);
  REQUIRE(db.has_value());
  CHECK(*db == std::filesystem::path{"/x/q.db"});
  CHECK(planar::buildqueue::queue_log_directory(*db) == std::filesystem::path{"/x/q-logs"});
}

TEST_CASE("path: with the override unset, the database falls back under HOME", "[buildqueue][path]") {
  auto const env = env_of({{"HOME", "/h"}});

  auto const db = planar::buildqueue::resolve_queue_db_path(env);
  REQUIRE(db.has_value());
  CHECK(*db == std::filesystem::path{"/h/.planar/build-queue.db"});
  CHECK(planar::buildqueue::queue_log_directory(*db) == std::filesystem::path{"/h/.planar/build-queue-logs"});
}

TEST_CASE("path: an empty override counts as unset and falls back under HOME", "[buildqueue][path]") {
  auto const env = env_of({{std::string{k_env_db}, ""}, {"HOME", "/h"}});

  auto const db = planar::buildqueue::resolve_queue_db_path(env);
  REQUIRE(db.has_value());
  CHECK(*db == std::filesystem::path{"/h/.planar/build-queue.db"});
}

TEST_CASE("path: with neither variable usable the error names both", "[buildqueue][path]") {
  auto const env = env_of({{"HOME", ""}});

  auto const db = planar::buildqueue::resolve_queue_db_path(env);
  REQUIRE_FALSE(db.has_value());
  CHECK(db.error() == error::no_location);
  auto const message = planar::buildqueue::error_message(db.error());
  CHECK_THAT(message, Catch::Matchers::ContainsSubstring(std::string{k_env_db}));
  CHECK_THAT(message, Catch::Matchers::ContainsSubstring("HOME"));
}

TEST_CASE("path: a database name without a .db suffix keeps its whole name for the log directory", "[buildqueue][path]") {
  CHECK(planar::buildqueue::queue_log_directory(std::filesystem::path{"/x/queue"}) == std::filesystem::path{"/x/queue-logs"});
  CHECK(planar::buildqueue::queue_log_directory(std::filesystem::path{"/x/q.db.bak"}) ==
        std::filesystem::path{"/x/q.db.bak-logs"});
}

TEST_CASE("path: the slot marker is PLANAR_BUILD_QUEUE_SLOT", "[buildqueue][path]") {
  CHECK(k_env_slot == "PLANAR_BUILD_QUEUE_SLOT");
  CHECK(k_env_db == "PLANAR_BUILD_QUEUE_DB");
}
