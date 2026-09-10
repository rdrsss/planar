// @file schema_columns.t.cpp
// @brief The column vocabulary a freshly migrated database actually
// declares, and the suffix rule anchored against it.
//
// ## Provenance (plan 996, task 6045; decisions 963/982)
//
// This case used to live in `statediff.t.cpp`, the C++/Zig DATABASE-STATE
// differential lane, where `is_volatile` decided which columns that lane
// BLANKED before comparing two databases. The lane and its oracle were
// deleted at the M10 cutover. This case survived it because it never
// consulted the oracle: it runs `init` once, reads the migrated schema back
// through `planar.db`, and asserts facts about THIS tree's schema.
//
// The subject that outlived the differential is the schema, not the diff.
// A stamp column renamed by a migration, a `checksum` column appearing, or
// `dirty_at_claim` disappearing are all real findings independent of any
// second implementation. `volatile_exact` / `is_volatile` are retained as
// the executable statement of the rule those facts anchor: `_at` is a
// SUFFIX rule, not a substring rule, and the seven exceptions are named.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Column names that are wall-clock-, process- or randomness-derived
/// and are NOT spelled with the `_at` suffix.
///
/// Deliberately short, and every entry is justified by CONSTRUCTION rather
/// than by observation. Entries:
///
///   `at`            `sync_events.at` is a wall-clock stamp whose name
///                   simply predates the `_at` convention. The suffix rule
///                   does not match a bare `at`, so it needs naming.
///   `fs_mtime`      `workbench_sync_state.fs_mtime` is a filesystem mtime.
///   `duration_ms`   `cli_invocations.duration_ms` is measured wall time.
///   `pid`           `workflow_runs.pid` is an OS process id.
///   `claim_token`   `agent_work_claims.claim_token` is randomly generated.
///   `run_uid`       `runs.run_uid` is randomly generated.
///   `preview_token` `routing_dispatch_previews.preview_token` likewise.
///
/// The prototype this list was ported from carried `execution_time_ms`,
/// which does not exist in this schema and therefore named nothing. That is
/// exactly the rot the case below now makes impossible.
constexpr std::array<std::string_view, 7> volatile_exact{"at",          "fs_mtime", "duration_ms",  "pid",
                                                         "claim_token", "run_uid",  "preview_token"};

/// @brief Whether a column's value is wall-clock-, process- or
/// randomness-derived rather than a stable planning value.
///
/// Every wall-clock stamp this schema declares is spelled `<something>_at`,
/// so the SUFFIX carries almost all of the work without a hand-maintained
/// list that silently goes stale as migrations add columns.
/// @param column The column name.
/// @return `true` when the column's value cannot be reproduced by a re-run.
auto is_volatile(std::string_view column) -> bool {
  if (column.ends_with("_at")) {
    return true;
  }
  return std::ranges::contains(volatile_exact, column);
}

} // namespace

TEST_CASE("the volatile-column exact list has no stale entries", "[cmd][schema][columns]") {
  // A guard on THIS FILE, not on any binary's behaviour. A name in
  // `volatile_exact` that no longer matches any column is an exception that
  // silently stopped applying — and the way that reads from outside is
  // "still green", which is the failure mode the list exists to avoid. The
  // prototype carried exactly one such entry (`execution_time_ms`).
  //
  // The schema comes from a freshly migrated scratch database, so this also
  // fails the day a migration renames one of the seven.
  auto const space = make_arena("schema_columns");
  auto const got =
      run_pinned(cpp_bin(), std::array<std::string, 3>{"init", "--skip-project", "--allow-no-repo"}, space.cpp_root, "vol");
  REQUIRE(got.code == 0);
  REQUIRE(std::filesystem::exists(space.cpp_root / "planar.db"));

  auto opened = planar::db::connection::open_read_only((space.cpp_root / "planar.db").string());
  REQUIRE(opened.has_value());

  std::set<std::string> declared;
  auto                  listing = opened->prepare("select name from sqlite_master where type = 'table' "
                                                  "and name not like 'sqlite_%' order by name");
  REQUIRE(listing.has_value());
  std::vector<std::string> tables;
  while (true) {
    auto stepped = listing->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    tables.push_back(listing->column_text(0));
  }
  REQUIRE_FALSE(tables.empty());
  for (auto const& table : tables) {
    auto info = opened->prepare(std::format(R"(pragma table_info("{}"))", table));
    if (!info) {
      continue;
    }
    while (true) {
      auto stepped = info->step();
      if (!stepped || *stepped == planar::db::step_result::done) {
        break;
      }
      declared.insert(info->column_text(1));
    }
  }

  for (auto const& name : volatile_exact) {
    INFO("volatile_exact entry: " << name);
    CHECK(declared.contains(std::string{name}));
  }

  // The counterexample that keeps the suffix rule ANCHORED. This schema
  // really does declare a column that CONTAINS `_at` and is not a stamp —
  // `agent_work_claims.dirty_at_claim`, a boolean recording whether the
  // worktree was dirty at claim time. A rule spelled `contains("_at")`
  // rather than `ends_with("_at")` would sweep it up; this pins the
  // anchoring against a column that actually exists.
  CHECK(declared.contains("dirty_at_claim"));
  CHECK_FALSE(is_volatile("dirty_at_claim"));
  CHECK(is_volatile("recorded_at"));

  // AND THE CLAIM THIS REPLACED, recorded because it was believed and is
  // false: the reconnaissance note that justified the suffix rule said
  // `checksum` is deliberately NOT volatile, "because migrations are shared
  // files, so a checksum divergence is a real finding". Planar's
  // `schema_migrations` is `(version, applied_at, description)` — there is
  // no `checksum` column anywhere in this schema or in any migration. That
  // is an sqlx-cli concept Planar does not use. The rule is right; the
  // reason given for it described a column that does not exist, and an
  // assertion resting on it fails.
  CHECK_FALSE(declared.contains("checksum"));
}
