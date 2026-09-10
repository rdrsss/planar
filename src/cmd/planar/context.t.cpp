// @file context.t.cpp
// @brief Tests for `planar.cmd.planar.context` (plan 996, task 6105).
//
// HOME / DB SAFETY. Nothing in this file reads the process environment.
// Every case builds its own `env_lookup` over an explicit map, which is the
// whole point of the design under test: a `context` cannot find the
// operator's real `~/.planar/planar.db` unless a map hands it one. The two
// cases that actually open SQLite point at a unique scratch directory under
// `std::filesystem::temp_directory_path()`.
//
// ORACLE PROVENANCE. `resolve_db_path`'s precedence is transcribed from
// zig/src/runtime/runtime.zig's `resolveDbPath` (PLANAR_DB, else
// HOME/.planar/planar.db, else error.HomeNotSet) and cross-checked against
// the oracle's observable behaviour: running the reference binary with
// PLANAR_DB pointed at a scratch path creates that file and leaves
// ~/.planar/planar.db untouched (the mtime check commit 3ec6c37 established).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.exit;
import planar.cmd.planar.context;

namespace {

using planar::cmd::context;
using planar::cmd::env_lookup;

/// @brief A unique scratch directory for one test case.
/// @param tag A short discriminator so a failure names its own case.
/// @return The created directory.
auto scratch_dir(std::string_view tag) -> std::filesystem::path {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_ctx_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  return root;
}

/// @brief A context over an explicit environment, writing to caller-owned
/// string streams.
/// @param vars The environment.
/// @param cwd The working directory.
/// @param db_path The database path.
/// @param out The stdout stream.
/// @param err The stderr stream.
/// @return The constructed context.
auto make_context(std::map<std::string, std::string, std::less<>> vars, std::filesystem::path cwd, std::filesystem::path db_path,
                  std::ostream& out, std::ostream& err) -> context {
  return context{{"planar"}, planar::cmd::map_env(std::move(vars)), std::move(cwd), std::move(db_path), out, err};
}

} // namespace

TEST_CASE("resolve_db_path prefers PLANAR_DB over HOME", "[cmd][context]") {
  auto const env = planar::cmd::map_env({{"PLANAR_DB", "/scratch/explicit.db"}, {"HOME", "/scratch/home"}});
  auto const got = planar::cmd::resolve_db_path(env);
  REQUIRE(got.has_value());
  CHECK(got->string() == "/scratch/explicit.db");
}

TEST_CASE("resolve_db_path falls back to HOME/.planar/planar.db", "[cmd][context]") {
  auto const env = planar::cmd::map_env({{"HOME", "/scratch/home"}});
  auto const got = planar::cmd::resolve_db_path(env);
  REQUIRE(got.has_value());
  CHECK(got->string() == "/scratch/home/.planar/planar.db");
}

TEST_CASE("resolve_db_path ignores PLANAR_HOME", "[cmd][context]") {
  // Not pedantry: CLAUDE.md calls reaching for PLANAR_HOME instead of
  // PLANAR_DB "a common and costly mistake", and the Zig original reads
  // PLANAR_DB then HOME and nothing else. A port that quietly honoured
  // PLANAR_HOME here would send every verb at a different database than
  // the reference binary uses under the same environment.
  auto const env = planar::cmd::map_env({{"PLANAR_HOME", "/scratch/planarhome"}, {"HOME", "/scratch/home"}});
  auto const got = planar::cmd::resolve_db_path(env);
  REQUIRE(got.has_value());
  CHECK(got->string() == "/scratch/home/.planar/planar.db");
}

TEST_CASE("resolve_db_path fails when neither PLANAR_DB nor HOME is set", "[cmd][context]") {
  auto const env = planar::cmd::map_env({});
  auto const got = planar::cmd::resolve_db_path(env);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == planar::cmd::domain_error_kind::generic_failure);
}

TEST_CASE("operator_cwd honours a PWD that really names this directory", "[cmd][context]") {
  // plan 351 task 2375: canonicalising resolves macOS's /var ->
  // /private/var symlink, which would stop a project registered under
  // /var/... from matching the projects.root_path key `assoc add` wrote.
  // So a VERIFIED PWD wins, spelling and all.
  //
  // The fixture is that exact shape built by hand: a symlink whose target is
  // the process's own directory. `realpath(link)` equals `realpath(".")`, so
  // the link spelling must survive — that is the whole point of preferring
  // PWD at all.
  auto const      target = scratch_dir("cwdreal");
  auto const      link   = target.parent_path() / (target.filename().string() + "-link");
  std::error_code ec;
  std::filesystem::create_directory_symlink(target, link, ec);
  if (ec) {
    SKIP("cannot create a directory symlink here");
  }

  auto const saved = std::filesystem::current_path();
  std::filesystem::current_path(target);
  auto const got = planar::cmd::operator_cwd(planar::cmd::map_env({{"PWD", link.string()}}));
  std::filesystem::current_path(saved);

  CHECK(got == link);
  CHECK(got != target);
}

TEST_CASE("operator_cwd IGNORES a stale inherited PWD", "[cmd][context][6132]") {
  // THE REGRESSION THIS FUNCTION SHIPPED WITH. `$PWD` is inherited, not
  // maintained by the kernel: a process spawned with an explicit cwd keeps
  // its PARENT's value. Returning it unconditionally made `planar init`
  // register `projects.root_path` as the parent's directory — exit 0,
  // plausible output, wrong row.
  //
  // The test that stood here before asserted the opposite, with a PWD
  // (`/var/somewhere/project`) that does not exist on any machine, so the
  // suite was green on the broken behaviour and three integration suites
  // (`repo_scope_test`'s stale-PWD, `--force` repoint and `--slug` repoint
  // cases) were the first thing to notice.
  auto const      here     = scratch_dir("cwdstale");
  auto const      elsewher = scratch_dir("cwdstale-parent");
  auto const      saved    = std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::current_path(here, ec);
  REQUIRE_FALSE(ec);
  auto const stale       = planar::cmd::operator_cwd(planar::cmd::map_env({{"PWD", elsewher.string()}}));
  auto const nonexistent = planar::cmd::operator_cwd(planar::cmd::map_env({{"PWD", "/var/somewhere/project"}}));
  auto const relative    = planar::cmd::operator_cwd(planar::cmd::map_env({{"PWD", "some/relative/path"}}));
  std::filesystem::current_path(saved);

  auto const identity = std::filesystem::canonical(here);
  CHECK(stale == identity);
  CHECK(stale != elsewher);
  // A PWD naming nothing at all cannot be verified either, so it loses too.
  CHECK(nonexistent == identity);
  // And a relative PWD is not a working directory in the first place.
  CHECK(relative == identity);
}

TEST_CASE("operator_cwd falls back to the real working directory when PWD is unset or empty", "[cmd][context]") {
  std::error_code ec;
  auto const      here = std::filesystem::current_path(ec);
  REQUIRE_FALSE(ec);
  auto const identity = std::filesystem::canonical(here);
  CHECK(planar::cmd::operator_cwd(planar::cmd::map_env({})) == identity);
  CHECK(planar::cmd::operator_cwd(planar::cmd::map_env({{"PWD", ""}})) == identity);
}

TEST_CASE("the database is not opened until ensure_db is called", "[cmd][context]") {
  // The lazy-acquisition rule is invisible in output, so the only way to
  // prove it holds is to assert the negative. If construction ever started
  // opening, `planar --help` would migrate whatever $PLANAR_DB points at.
  auto const         root    = scratch_dir("lazy");
  auto const         db_path = root / "planar.db";
  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context({}, root, db_path, out, err);

  CHECK_FALSE(ctx.db_opened());
  CHECK_FALSE(std::filesystem::exists(db_path));

  auto conn = ctx.ensure_db();
  REQUIRE(conn.has_value());
  CHECK(ctx.db_opened());
  CHECK(std::filesystem::exists(db_path));
}

TEST_CASE("ensure_db creates the parent directory, migrates, and caches the handle", "[cmd][context]") {
  auto const         root    = scratch_dir("migrate");
  auto const         db_path = root / "nested" / "planar.db";
  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context({}, root, db_path, out, err);

  auto first = ctx.ensure_db();
  REQUIRE(first.has_value());

  auto const version = planar::db::current_version(**first);
  REQUIRE(version.has_value());
  std::uint32_t embedded_max = 0;
  for (auto const& record : planar::db::migrations()) {
    embedded_max = std::max(embedded_max, record.version_);
  }
  CHECK(*version == embedded_max);

  auto second = ctx.ensure_db();
  REQUIRE(second.has_value());
  CHECK(*first == *second);
}

TEST_CASE("ensure_db refuses a database migrated past this binary's chain", "[cmd][context]") {
  // The SchemaVersionAhead guard, mapped to exit 7 on the `planar` binary
  // (planar.cli.exit). Simulated by inserting a schema_migrations row above
  // the embedded maximum after a normal migrate, then reopening — which is
  // exactly the state a newer binary leaves behind.
  auto const root    = scratch_dir("ahead");
  auto const db_path = root / "planar.db";
  {
    std::ostringstream out;
    std::ostringstream err;
    auto               ctx  = make_context({}, root, db_path, out, err);
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    std::uint32_t embedded_max = 0;
    for (auto const& record : planar::db::migrations()) {
      embedded_max = std::max(embedded_max, record.version_);
    }
    auto const inserted = (*conn)->execute(std::format(
        "insert into schema_migrations (version, description) values ({}, 'from a newer binary');", embedded_max + 1));
    REQUIRE(inserted.has_value());
  }

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx  = make_context({}, root, db_path, out, err);
  auto               conn = ctx.ensure_db();
  REQUIRE_FALSE(conn.has_value());
  CHECK(conn.error().kind == planar::cmd::domain_error_kind::schema_version_ahead);
  CHECK(planar::cmd::exit_code_for(conn.error().kind) == 7);
  // The handle is dropped rather than handed back half-usable.
  CHECK_FALSE(ctx.db_opened());
}
