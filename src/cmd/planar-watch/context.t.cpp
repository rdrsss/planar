// @file context.t.cpp
// @brief LEVEL 2 of `planar-watch`'s read-only guarantee: the handle
// itself, proven by making a write through it FAIL (plan 996, task 6107).
//
// ## Why these tests are shaped the way they are
//
// The easy version of this file would assert `is_read_only()` and stop.
// That proves the connection remembers a flag it was constructed with —
// nothing about whether SQLite will actually refuse a write. It would pass
// against a handle opened read/write and mislabelled, which is exactly the
// regression this invariant is exposed to.
//
// So the load-bearing case below EXECUTES A REAL INSERT through the handle
// `context::ensure_db()` hands out and REQUIRES IT TO FAIL. That is the
// "second line of defense behind this binary's `no write verbs registered`
// capability boundary" its own `--help` page promises operators, tested as
// a behaviour rather than as a label.
//
// The second load-bearing case is the one that discriminates this context
// from the OPERATOR binary's: pointed at a path that does not exist, this
// one must FAIL AND CREATE NOTHING, where `planar.cmd.planar.context`
// would create the parent directory, create the file, and migrate it. A
// viewer that bootstrapped state would not be a viewer. Quoting
// zig/src/runtime/runtime.zig: "A read-only viewer running before `planar
// init` should fail loudly with a schema-handshake error, not silently
// create empty state."
//
// HOME / DB SAFETY. Every case builds its own environment map over a unique
// scratch root; nothing here calls std::getenv.
//
// ## Break-probes run against this file
//
//   - Changed `db::connection::open_read_only` to `db::connection::open` in
//     context.cpp -> `a write through the handle is REFUSED BY SQLITE`
//     FAILS (three assertions: the create, the insert, and the reopened
//     probe). Restored -> green.
//
//     THAT PROBE ALSO FOUND A SURVIVOR, and the fix is why the
//     not-created cases below are shaped the way they are. The original
//     single case pointed the context at a path whose PARENT DIRECTORY was
//     also missing — and a plain read/write `open` fails there too, because
//     SQLite cannot create a file in a directory that does not exist. So
//     the case passed under the mutation and proved nothing about
//     read-only-ness at all. It is now split in two: `a missing database
//     FILE is not created` uses an EXISTING parent, where a writable open
//     really would create the file (and now fails under the mutation), and
//     `a missing parent DIRECTORY is not created` covers the separate
//     create_directories question.
//   - Added a `create_directories` call before the open (planar-agent's and
//     the operator binary's behaviour) -> `a missing parent DIRECTORY is
//     not created` FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;

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
                    std::format("planar_cmd_watch_ctx_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Construct a context over `fx` with an explicit database path.
/// @param fx The fixture.
/// @param db_path The database path to use.
/// @param out The stdout sink.
/// @param err The stderr sink.
/// @return The context.
auto make_context(const fixture& fx, const std::filesystem::path& db_path, std::ostream& out, std::ostream& err)
    -> planar::cmd::watch::context {
  return planar::cmd::watch::context{
      std::vector<std::string>{"planar-watch"}, planar::cmd::watch::map_env(fx.vars), fx.root / "proj", db_path, out, err};
}

/// @brief Create a fully-migrated database at `path`, the way `planar init`
/// would — planar-watch cannot, which is the point.
/// @param path Where to create it.
auto seed_migrated_db(const std::filesystem::path& path) -> void {
  auto opened = planar::db::connection::open(path.string());
  REQUIRE(opened.has_value());
  auto applied = planar::db::apply_all(*opened);
  REQUIRE(applied.has_value());
}

} // namespace

TEST_CASE("planar-watch: a write through the handle is REFUSED BY SQLITE", "[cmd][watch][context][readonly]") {
  auto const fx = make_fixture("refuseswrite");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, fx.db_path, out, err);

  auto const opened = ctx.ensure_db();
  REQUIRE(opened.has_value());
  auto* conn = *opened;

  // A read works — the handle is live, not merely broken. Without this the
  // "write fails" assertion below would also pass against a dead handle.
  auto const read = conn->prepare("select count(*) from schema_migrations");
  CHECK(read.has_value());

  // THE ASSERTION. Not `is_read_only()` — an actual write, actually
  // refused, by the driver.
  auto const created = conn->execute("create table watch_write_probe (id integer primary key)");
  CHECK_FALSE(created.has_value());

  auto const inserted = conn->execute("insert into schema_migrations (version, description) values (99999, 'probe')");
  CHECK_FALSE(inserted.has_value());

  // The label agrees with the behaviour (checked second, and only after
  // the behaviour, so it can never stand in for it).
  CHECK(conn->is_read_only());

  // And the database is genuinely unchanged: no probe table exists when
  // reopened through an independent read/write connection.
  auto reopened = planar::db::connection::open(fx.db_path.string());
  REQUIRE(reopened.has_value());
  auto probe = reopened->prepare("select count(*) from sqlite_master where name = 'watch_write_probe'");
  REQUIRE(probe.has_value());
  auto const stepped = probe->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  CHECK(probe->column_int64(0) == 0);
}

TEST_CASE("planar-watch: a missing database FILE is not created", "[cmd][watch][context][readonly]") {
  auto const fx = make_fixture("nocreatefile");
  // The parent directory EXISTS and the file does not. That combination is
  // load-bearing and was found by break-probe: with a MISSING parent, a
  // plain read/write `open` also fails (SQLite cannot create a file in a
  // directory that is not there), so the case passed under the mutation
  // and proved nothing. Here a read/write open would succeed and create
  // the file, so the assertion genuinely discriminates.
  auto const missing = fx.root / "proj" / "absent.db";
  REQUIRE(std::filesystem::exists(missing.parent_path()));
  REQUIRE_FALSE(std::filesystem::exists(missing));

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, missing, out, err);

  auto const opened = ctx.ensure_db();
  CHECK_FALSE(opened.has_value());
  CHECK_FALSE(ctx.db_opened());
  CHECK_FALSE(std::filesystem::exists(missing));
}

TEST_CASE("planar-watch: a missing parent DIRECTORY is not created", "[cmd][watch][context][readonly]") {
  auto const fx      = make_fixture("nocreatedir");
  auto const missing = fx.root / "never" / "created" / "planar.db";

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, missing, out, err);

  // The operator binary's context AND planar-agent's both call
  // `create_directories` here; only the strict read-only path does not.
  // zig/src/runtime/runtime.zig: "A read-only viewer running before
  // `planar init` should fail loudly with a schema-handshake error, not
  // silently create empty state."
  auto const opened = ctx.ensure_db();
  CHECK_FALSE(opened.has_value());
  CHECK_FALSE(ctx.db_opened());
  CHECK_FALSE(std::filesystem::exists(missing.parent_path()));
}

TEST_CASE("planar-watch: an unmigrated database is refused with exit 7 and a remediation line", "[cmd][watch][context]") {
  auto const fx = make_fixture("behind");
  // An empty-but-present file: schema version 0, the "fresh DB never
  // touched by `planar init`" case.
  {
    std::ofstream create(fx.db_path, std::ios::binary);
  }

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, fx.db_path, out, err);

  auto const opened = ctx.ensure_db();
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::watch::domain_error_kind::schema_version_behind);
  // 7 here, where the OPERATOR binary maps the same kind to 1 (it has no
  // SchemaVersionBehind arm at all) — plan 996 task 6066's finding.
  CHECK(planar::cmd::watch::exit_code(opened.error()) == 7);
  CHECK(err.str().contains("run `planar init` to apply migrations"));
  CHECK_FALSE(ctx.db_opened());
}

TEST_CASE("planar-watch: ensure_db caches the SAME read-only handle", "[cmd][watch][context][readonly]") {
  auto const fx = make_fixture("cached");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, fx.db_path, out, err);

  auto const first = ctx.ensure_db();
  REQUIRE(first.has_value());
  auto const second = ctx.ensure_db();
  REQUIRE(second.has_value());
  // Same handle, so a second caller cannot end up with a writable one —
  // the Zig original's "The cached handle IS the strict read-only one; any
  // caller in this process that retrieves it gets the same write-refusing
  // connection."
  CHECK(*first == *second);
  CHECK((*second)->is_read_only());
}

TEST_CASE("planar-watch: no WAL pragma warning is emitted on a healthy open", "[cmd][watch][context]") {
  auto const fx = make_fixture("nowal");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, fx.db_path, out, err);
  REQUIRE(ctx.ensure_db().has_value());

  // `journal_mode = WAL` is writer-side configuration a read-only handle
  // cannot set. Copying the agent context's PRAGMA pair verbatim would
  // print a warning on EVERY invocation of a healthy viewer — a
  // regression invisible to any output-equality test that only looks at
  // stdout.
  CHECK(err.str().empty());
}
