// @file context.t.cpp
// @brief The `planar-agent` CONSUMER database policy, proven by what it
// refuses to do (plan 996, task 6107).
//
// The interesting assertions here are all NEGATIVE, because the policy is
// defined by absence: this binary does NOT migrate. A test that only
// checked "ensure_db returns a handle against a healthy database" would
// pass identically against the operator binary's migrating context and
// would therefore prove nothing about the boundary.
//
// HOME / DB SAFETY. Every case builds its own environment map over a unique
// scratch root and passes the database path to the context directly.
// Nothing here calls std::getenv, so there is no inherited-environment path
// by which the operator's ~/.planar could be reached.
//
// ## Break-probes run against this file
//
//   - Added `db::apply_all(*_db)` to `context::ensure_db` (i.e. made this
//     binary migrate like the operator one) -> `refuses a database it
//     would have to migrate` FAILS: the call now succeeds where it must
//     refuse. Restored -> green.
//   - Changed the `stored < maximum` refusal to fall through -> same test
//     FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;

namespace {

/// @brief A scratch root plus the environment map a case dispatches under.
struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_cmd_agent_ctx_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Construct a context over `fx`, writing to `out`/`err`.
/// @param fx The fixture.
/// @param out The stdout sink.
/// @param err The stderr sink.
/// @return The context.
auto make_context(const fixture& fx, std::ostream& out, std::ostream& err) -> planar::cmd::agent::context {
  return planar::cmd::agent::context{
      std::vector<std::string>{"planar-agent"}, planar::cmd::agent::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
}

/// @brief Create a fully-migrated database at `path`, the way `planar init`
/// would — this binary cannot do it itself, which is the point.
/// @param path Where to create it.
auto seed_migrated_db(const std::filesystem::path& path) -> void {
  auto opened = planar::db::connection::open(path.string());
  REQUIRE(opened.has_value());
  auto applied = planar::db::apply_all(*opened);
  REQUIRE(applied.has_value());
}

} // namespace

TEST_CASE("planar-agent refuses a database it would have to migrate", "[cmd][agent][context]") {
  auto const         fx = make_fixture("behind");
  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, out, err);

  // A nonexistent path opens as a brand-new, EMPTY database — schema
  // version 0 — which is exactly the "fresh DB never touched by `planar
  // init`" case zig/src/runtime/runtime.zig calls out.
  auto const opened = ctx.ensure_db();
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::agent::domain_error_kind::schema_version_behind);

  // Exit 7, and this is the second per-binary divergence task 6066 found:
  // `planar` has NO SchemaVersionBehind arm and falls through to 1.
  CHECK(planar::cmd::agent::exit_code(opened.error()) == 7);

  // The remediation line reaches the operator, not just the error tag.
  CHECK(err.str().contains("run `planar init` to apply migrations"));
  CHECK(err.str().contains("is older than this binary's minimum of"));

  // The handle was released rather than left open in a refused state.
  CHECK_FALSE(ctx.db_opened());

  // AND THE DECISIVE PART: it did not migrate on the way out. The file it
  // opened is still at version 0.
  auto reopened = planar::db::connection::open(fx.db_path.string());
  REQUIRE(reopened.has_value());
  auto const version = planar::db::current_version(*reopened);
  CHECK((!version.has_value() || *version == 0));
}

TEST_CASE("planar-agent accepts a database already at the embedded version", "[cmd][agent][context]") {
  auto const fx = make_fixture("ok");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, out, err);

  auto const opened = ctx.ensure_db();
  REQUIRE(opened.has_value());
  CHECK(ctx.db_opened());
  CHECK(err.str().empty());

  // Read/write at the driver level — this binary DOES write
  // (agent_actions, agent_work_claims, ...). Contrast planar-watch, whose
  // context.t.cpp requires the symmetric write to FAIL.
  CHECK_FALSE((*opened)->is_read_only());
  auto const wrote = (*opened)->execute("create table cap_probe (id integer primary key)");
  CHECK(wrote.has_value());
}

TEST_CASE("planar-agent's ensure_db is idempotent and caches", "[cmd][agent][context]") {
  auto const fx = make_fixture("cached");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, out, err);

  auto const first = ctx.ensure_db();
  REQUIRE(first.has_value());
  auto const second = ctx.ensure_db();
  REQUIRE(second.has_value());
  CHECK(*first == *second);
}

TEST_CASE("planar-agent resolves the database path PLANAR_DB-first, never PLANAR_HOME", "[cmd][agent][context]") {
  // CLAUDE.md calls mistaking PLANAR_HOME for PLANAR_DB "a common and
  // costly mistake"; the Zig original reads PLANAR_DB then HOME and
  // nothing else. A context that consulted PLANAR_HOME would find the
  // WRONG file on every real operator machine, where both are set.
  auto const explicit_db = planar::cmd::agent::resolve_db_path(planar::cmd::agent::map_env(
      {{"PLANAR_DB", "/tmp/explicit.db"}, {"PLANAR_HOME", "/tmp/some-home"}, {"HOME", "/tmp/real-home"}}));
  REQUIRE(explicit_db.has_value());
  CHECK(explicit_db->string() == "/tmp/explicit.db");

  auto const from_home = planar::cmd::agent::resolve_db_path(
      planar::cmd::agent::map_env({{"PLANAR_HOME", "/tmp/some-home"}, {"HOME", "/tmp/real-home"}}));
  REQUIRE(from_home.has_value());
  CHECK(from_home->string() == "/tmp/real-home/.planar/planar.db");

  auto const neither = planar::cmd::agent::resolve_db_path(planar::cmd::agent::map_env({}));
  REQUIRE_FALSE(neither.has_value());
}

TEST_CASE("planar-agent's operator cwd is PWD-first", "[cmd][agent][context]") {
  // Canonicalising resolves macOS's /var -> /private/var symlink, which
  // would break cwd-derived scope against a projects.root_path recorded
  // under /var (plan 351 task 2375).
  CHECK(planar::cmd::agent::operator_cwd(planar::cmd::agent::map_env({{"PWD", "/var/project"}})).string() == "/var/project");
  // An empty PWD falls through rather than yielding an empty path.
  CHECK_FALSE(planar::cmd::agent::operator_cwd(planar::cmd::agent::map_env({{"PWD", ""}})).empty());
}
