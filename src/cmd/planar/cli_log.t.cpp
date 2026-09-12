// @file cli_log.t.cpp
// @brief The cli_invocations capture hook, and above all its PRIVACY
// INVARIANT (plan 996, task 6073).
//
// ## What these cases are actually defending
//
// `cli_invocations` is a local database of what the operator typed. The
// contract that makes that acceptable is that flag and positional VALUES
// never enter it -- only shapes. A port that gets this wrong does not fail
// loudly; it writes plausible rows containing an operator's titles, bodies,
// slugs and tokens, forever, on a path that swallows all its own errors.
//
// So the leak cases below do not assert a rendering. They assert that a
// SENTINEL the test put into a value is ABSENT from what was recorded, and
// the strongest of them checks the database ROW rather than the in-memory
// shape -- because "the shape was clean" and "nothing leaked" are different
// claims, and only the second one is the contract.
//
// ## Every expectation here was produced by RUNNING the oracle
//
// The table in cli_log.cppm's header is the transcript: the built
// zig/zig-out/bin/planar was run against a scratch database with
// `[introspection] cli_log = true`, and `cli_invocations` was read back
// argv by argv. The `resume SENTINEL` / `tree SENTINEL` rows below are from
// that run, not from reading the Zig source -- which matters, because the
// Zig source's own comment about the `--flag=value` form contradicts the
// Zig code, and the oracle rows settle it in favour of the code.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar.cli_log;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace {

namespace pc = planar::cmd;

/// @brief Build an argv tail from a braced list.
/// @param tokens The tokens.
/// @return The owning vector.
auto tail(std::initializer_list<std::string_view> tokens) -> std::vector<std::string> {
  return std::vector<std::string>{tokens.begin(), tokens.end()};
}

/// @brief Shape one argv tail.
/// @param tokens The tokens after argv[0].
/// @return The parsed shape.
auto shape_of(std::initializer_list<std::string_view> tokens) -> pc::parsed_args_shape {
  auto const owned = tail(tokens);
  return pc::parse_args(owned);
}

/// @brief A scratch directory holding a migrated database.
class scratch_db {
public:
  scratch_db()
      : _dir(std::filesystem::temp_directory_path() /
             std::format("planar_cli_log_{}", std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::error_code ec;
    std::filesystem::create_directories(_dir, ec);
  }
  scratch_db(const scratch_db&)                    = delete;
  auto operator=(const scratch_db&) -> scratch_db& = delete;
  ~scratch_db() {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }

  /// @brief The database path.
  /// @return Its path.
  [[nodiscard]] auto path() const -> std::filesystem::path {
    return _dir / "planar.db";
  }
  /// @brief The scratch directory.
  /// @return Its path.
  [[nodiscard]] auto dir() const -> const std::filesystem::path& {
    return _dir;
  }

  /// @brief Open and migrate the database.
  /// @return The open, migrated connection.
  [[nodiscard]] auto open() const -> planar::db::connection {
    auto conn = planar::db::connection::open(path().string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    return std::move(*conn);
  }

private:
  std::filesystem::path _dir;
};

/// @brief Every `verb_path` and `args_shape` currently in the table, joined
/// with a space per row.
/// @param conn The connection to read.
/// @return One string per row.
auto recorded_text(planar::db::connection& conn) -> std::vector<std::string> {
  auto stmt = conn.prepare("select verb_path, args_shape from cli_invocations order by id");
  REQUIRE(stmt.has_value());
  std::vector<std::string> rows;
  for (;;) {
    auto const step = stmt->step();
    REQUIRE(step.has_value());
    if (*step != planar::db::step_result::row) {
      break;
    }
    rows.push_back(stmt->column_text(0) + " " + stmt->column_text(1));
  }
  return rows;
}

} // namespace

// --- the privacy invariant ------------------------------------------------

TEST_CASE("a flag value never reaches args_shape, in any of its four spellings", "[cmd][cli_log][privacy]") {
  constexpr std::string_view sentinel = "SENTINEL_MUST_NOT_LEAK";

  // 1. Separate following token. Consumed as the value and discarded --
  //    note it is not counted as a positional either.
  auto const separate = shape_of({"task", "add", "--title", sentinel, "--json"});
  CHECK(separate.verb_path == "task add");
  CHECK(separate.args_shape == "--title --json");

  // 2. The inline form. The recorded name EXCLUDES the `=`, which is what
  //    the oracle writes (`task show --plan=X` recorded `--plan`).
  auto const inline_form = shape_of({"task", "show", std::string_view{"--plan=SENTINEL_MUST_NOT_LEAK"}});
  CHECK(inline_form.verb_path == "task show");
  CHECK(inline_form.args_shape == "--plan");

  // 3 and 4. Short flag with an attached value, with and without `=`. No
  //    planar flag declares a short form today; the closure is structural,
  //    because the invariant has to hold for whatever argv arrives.
  auto const short_attached = shape_of({"task", "add", std::string_view{"-pSENTINEL_MUST_NOT_LEAK"}});
  CHECK(short_attached.args_shape == "-p");
  auto const short_equals = shape_of({"task", "add", std::string_view{"-p=SENTINEL_MUST_NOT_LEAK"}});
  CHECK(short_equals.args_shape == "-p");

  for (auto const& shape : {separate, inline_form, short_attached, short_equals}) {
    CHECK_FALSE(shape.args_shape.contains(sentinel));
    CHECK_FALSE(shape.verb_path.contains(sentinel));
  }
}

TEST_CASE("a positional value is counted, never recorded", "[cmd][cli_log][privacy]") {
  constexpr std::string_view sentinel = "SENTINEL_MUST_NOT_LEAK";

  // Beyond the verb depth.
  auto const one = shape_of({"plan", "show", sentinel});
  CHECK(one.verb_path == "plan show");
  CHECK(one.args_shape == "<pos:1>");

  // After the `--` separator, all of them.
  auto const after_separator = shape_of({"task", "add", "--", sentinel, "second", "third"});
  CHECK(after_separator.verb_path == "task add");
  CHECK(after_separator.args_shape == "<pos:3>");

  // Mixed: a flag consumes its value, and the trailing bare word counts.
  auto const mixed = shape_of({"plan", "show", "--scope", sentinel, "trailing"});
  CHECK(mixed.args_shape == "<pos:1> --scope");

  for (auto const& shape : {one, after_separator, mixed}) {
    CHECK_FALSE(shape.args_shape.contains(sentinel));
    CHECK_FALSE(shape.verb_path.contains(sentinel));
  }
}

TEST_CASE("a hostile value cannot reach the cli_invocations ROW", "[cmd][cli_log][privacy]") {
  // The one that is actually the contract. The cases above check the
  // in-memory shape; this one writes through the real insert and reads the
  // stored bytes back, so a leak introduced anywhere between parse_args and
  // SQLite -- a stray verbatim bind, a "helpful" extra column, a format
  // string that interpolated argv -- is caught rather than assumed away.
  constexpr std::string_view sentinel = "SENTINEL_MUST_NOT_LEAK";

  scratch_db arena;
  auto       conn = arena.open();

  auto const hostile = tail({"task", "add", "--title", "SENTINEL_MUST_NOT_LEAK", "--body", "line one\nline two", "--plan",
                             "SENTINEL_MUST_NOT_LEAK", "--json", "SENTINEL_MUST_NOT_LEAK"});
  auto const shape   = pc::parse_args(hostile);
  REQUIRE(pc::write_invocation(conn, shape, 2, pc::category_for(planar::cmd::domain_error_kind::invalid_input), 12, 90));

  auto const rows = recorded_text(conn);
  REQUIRE(rows.size() == 1);
  CHECK_FALSE(rows[0].contains(sentinel));
  CHECK_FALSE(rows[0].contains("line one"));
  CHECK(rows[0] == "task add --title --body --plan --json");

  // The non-shape columns came through, so the row is a real record rather
  // than an empty one that trivially contains no secret.
  auto stmt = conn.prepare("select exit_code, error_category, duration_ms, scope_slug from cli_invocations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 2);
  CHECK(stmt->column_text(1) == "usage");
  CHECK(stmt->column_int64(2) == 12);
  CHECK(stmt->is_null(3)); // scope_slug is written as NULL, matching the oracle.
}

TEST_CASE("the verb slot IS recorded verbatim, and that is the oracle's boundary", "[cmd][cli_log][privacy]") {
  // Pinned in the OPPOSITE direction from the cases above, on purpose.
  //
  // The stated invariant is "flag VALUES are never recorded", and it says
  // nothing about the verb slot. Running the oracle shows why that wording
  // is exact: a ONE-level verb that takes a positional puts the operator's
  // argument straight into verb_path.
  //
  //     $ planar resume SENTINEL   -> verb_path 'resume SENTINEL'
  //     $ planar tree SENTINEL     -> verb_path 'tree SENTINEL'
  //
  // A porter who read the invariant as "no values are ever recorded" would
  // suppress these and diverge from the oracle on every `resume` and
  // `tree`. This case exists so that neither reading can drift silently: if
  // the project ever decides the oracle is wrong here, this test is where
  // the decision gets made, rather than somewhere a shape quietly changed.
  auto const resumed = shape_of({"resume", "6073"});
  CHECK(resumed.verb_path == "resume 6073");
  CHECK(resumed.args_shape.empty());

  auto const treed = shape_of({"tree", "plan:42"});
  CHECK(treed.verb_path == "tree plan:42");
  CHECK(treed.args_shape.empty());

  // The third non-flag token is past the depth limit and so IS suppressed,
  // which is what bounds the exposure to the two-token slot.
  auto const deep = shape_of({"resume", "6073", "SENTINEL_MUST_NOT_LEAK"});
  CHECK(deep.verb_path == "resume 6073");
  CHECK(deep.args_shape == "<pos:1>");
  CHECK_FALSE(deep.verb_path.contains("SENTINEL"));
}

// --- the rest of the shape contract --------------------------------------

TEST_CASE("parse_args reproduces the oracle's rows for the ordinary shapes", "[cmd][cli_log]") {
  // Straight from the derivation run; see this file's header.
  CHECK(shape_of({"health"}).verb_path == "health");
  CHECK(shape_of({"health"}).args_shape.empty());

  auto const task_show = shape_of({"task", "show", "6073", "--json"});
  CHECK(task_show.verb_path == "task show");
  CHECK(task_show.args_shape == "<pos:1> --json");

  auto const unknown = shape_of({"nosuchverb", "--json"});
  CHECK(unknown.verb_path == "nosuchverb");
  CHECK(unknown.args_shape == "--json");

  // Empty argv tail: no verb, no shape, and no crash.
  auto const nothing = pc::parse_args({});
  CHECK(nothing.verb_path.empty());
  CHECK(nothing.args_shape.empty());

  // A flag whose value LOOKS like a flag is not consumed as a value -- it
  // is recorded as a flag name in its own right, exactly as the oracle does.
  auto const flaggy = shape_of({"plan", "show", "--scope", "--json"});
  CHECK(flaggy.args_shape == "--scope --json");
}

TEST_CASE("category_for maps every domain-error kind the way the oracle does", "[cmd][cli_log]") {
  using k = planar::cmd::domain_error_kind;
  CHECK(pc::category_for(k::invalid_input) == "usage");
  CHECK(pc::category_for(k::invalid_entity_ref) == "usage");
  CHECK(pc::category_for(k::parse_error) == "usage");
  CHECK(pc::category_for(k::scope_mismatch) == "scope");
  CHECK(pc::category_for(k::not_found) == "not_found");
  CHECK(pc::category_for(k::sync_conflict) == "conflict");
  CHECK(pc::category_for(k::slug_conflict) == "conflict");
  CHECK(pc::category_for(k::already_exists) == "conflict");
  CHECK(pc::category_for(k::schema_version_ahead) == "internal");
  CHECK(pc::category_for(k::not_implemented) == "internal");
  CHECK(pc::category_for(k::generic_failure) == "internal");
  CHECK(pc::category_for(k::schema_version_behind) == "internal");
  CHECK(pc::category_for(k::busy_source) == "busy");

  // Every value it can return has to satisfy the migration's CHECK
  // constraint, or the insert fails and the row is silently lost.
  scratch_db arena;
  auto       conn = arena.open();
  for (auto const kind : {k::invalid_input, k::invalid_entity_ref, k::parse_error, k::scope_mismatch, k::not_found,
                          k::sync_conflict, k::slug_conflict, k::already_exists, k::schema_version_ahead, k::not_implemented,
                          k::generic_failure, k::schema_version_behind, k::busy_source}) {
    INFO("kind ordinal: " << static_cast<int>(kind));
    CHECK(pc::write_invocation(conn, pc::parsed_args_shape{.verb_path = "health"}, 1, pc::category_for(kind), std::nullopt, 90));
  }
}

TEST_CASE("a successful invocation records no error category", "[cmd][cli_log]") {
  // The migration's CHECK is `(exit_code = 0) = (error_category is null)`,
  // so getting this backwards loses the row rather than mislabelling it.
  scratch_db arena;
  auto       conn = arena.open();
  REQUIRE(pc::write_invocation(conn, pc::parsed_args_shape{.verb_path = "health"}, 0, std::nullopt, 5, 90));

  auto stmt = conn.prepare("select error_category, duration_ms from cli_invocations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->is_null(0));
  CHECK(stmt->column_int64(1) == 5);
}

TEST_CASE("write_invocation prunes rows past the retention window", "[cmd][cli_log]") {
  scratch_db arena;
  auto       conn = arena.open();

  REQUIRE(conn.execute("insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) "
                       "values ('ancient', '', 0, '2000-01-01T00:00:00.000Z')")
              .has_value());
  REQUIRE(pc::write_invocation(conn, pc::parsed_args_shape{.verb_path = "health"}, 0, std::nullopt, std::nullopt, 90));

  auto const rows = recorded_text(conn);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].starts_with("health"));

  // A generous window keeps everything: the prune is driven by the
  // configured retention, not by a hardcoded cutoff.
  REQUIRE(conn.execute("insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) "
                       "values ('ancient', '', 0, '2000-01-01T00:00:00.000Z')")
              .has_value());
  REQUIRE(pc::write_invocation(conn, pc::parsed_args_shape{.verb_path = "health"}, 0, std::nullopt, std::nullopt, 1000000));
  auto const kept = recorded_text(conn);
  REQUIRE(kept.size() == 3);
  CHECK(std::ranges::any_of(kept, [](auto const& row) { return row.starts_with("ancient"); }));
}

// --- record()'s fail-open and no-side-effect arms -------------------------

TEST_CASE("record never creates a database, and never records with logging off", "[cmd][cli_log]") {
  // Both arms are NEGATIVES that produce no output at all, so the only way
  // to check them is to assert on the filesystem and the table. The
  // create-the-database arm in particular was a live oracle hazard: a bare
  // `planar --help` on a fresh machine must not leave an empty database
  // behind purely as a side effect of telemetry.
  scratch_db arena;
  auto const cfg_on = arena.dir() / "config.toml";
  {
    std::ofstream file(cfg_on);
    file << "[introspection]\ncli_log = true\n";
  }

  auto const env = pc::map_env({{"HOME", arena.dir().string()}, {"PLANAR_CONFIG_PATH", cfg_on.string()}});

  // No database file yet. Logging is ON, so this is the arm that matters.
  {
    std::ostringstream out;
    std::ostringstream err;
    pc::context        ctx{{"planar", "health"}, env, arena.dir(), arena.path(), out, err};
    pc::record(ctx, 0, std::nullopt, std::chrono::milliseconds{3});
    CHECK_FALSE(std::filesystem::exists(arena.path()));
    CHECK(out.str().empty()); // fail-open: never speaks
    CHECK(err.str().empty());
  }

  // Now the database exists and logging is on: a row lands.
  {
    auto conn = arena.open();
    static_cast<void>(conn);
  }
  {
    std::ostringstream out;
    std::ostringstream err;
    pc::context ctx{{"planar", "task", "show", "SENTINEL_MUST_NOT_LEAK", "--json"}, env, arena.dir(), arena.path(), out, err};
    pc::record(ctx, 1, planar::cmd::domain_error_kind::not_found, std::chrono::milliseconds{7});
  }
  {
    auto       conn = arena.open();
    auto const rows = recorded_text(conn);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == "task show <pos:1> --json");
    CHECK_FALSE(rows[0].contains("SENTINEL"));
  }

  // With logging OFF, nothing more is written -- and note the default is
  // off, so an absent config file takes this same arm.
  auto const cfg_off = arena.dir() / "off.toml";
  {
    std::ofstream file(cfg_off);
    file << "[introspection]\ncli_log = false\n";
  }
  {
    auto const         off_env = pc::map_env({{"HOME", arena.dir().string()}, {"PLANAR_CONFIG_PATH", cfg_off.string()}});
    std::ostringstream out;
    std::ostringstream err;
    pc::context        ctx{{"planar", "health"}, off_env, arena.dir(), arena.path(), out, err};
    pc::record(ctx, 0, std::nullopt, std::chrono::milliseconds{1});
  }
  {
    auto conn = arena.open();
    CHECK(recorded_text(conn).size() == 1);
  }
}

TEST_CASE("resolve_config_path follows PLANAR_CONFIG_PATH then HOME", "[cmd][cli_log]") {
  CHECK(pc::resolve_config_path(pc::map_env({{"HOME", "/h"}})) == std::filesystem::path{"/h/.planar/config.toml"});
  CHECK(pc::resolve_config_path(pc::map_env({{"HOME", "/h"}, {"PLANAR_CONFIG_PATH", "/explicit.toml"}})) ==
        std::filesystem::path{"/explicit.toml"});

  // A leading `~` expands, both bare and as a prefix.
  CHECK(pc::resolve_config_path(pc::map_env({{"HOME", "/h"}, {"PLANAR_CONFIG_PATH", "~"}})) == std::filesystem::path{"/h"});
  CHECK(pc::resolve_config_path(pc::map_env({{"HOME", "/h"}, {"PLANAR_CONFIG_PATH", "~/c.toml"}})) ==
        std::filesystem::path{"/h/c.toml"});

  // An EMPTY override is treated as unset, matching the oracle's `raw.len > 0`
  // guard -- otherwise `PLANAR_CONFIG_PATH=` would resolve to the cwd.
  CHECK(pc::resolve_config_path(pc::map_env({{"HOME", "/h"}, {"PLANAR_CONFIG_PATH", ""}})) ==
        std::filesystem::path{"/h/.planar/config.toml"});

  // Neither variable: no path, and the caller logs nothing.
  CHECK_FALSE(pc::resolve_config_path(pc::map_env({})).has_value());
}
