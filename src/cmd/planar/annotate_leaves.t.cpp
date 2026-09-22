// @file annotate_leaves.t.cpp
// @brief In-process tests for the twelve `annotate` leaves wired by plan
// 996, task 6148 — `show`, `update`, `remove`, `tag`, `resolve`,
// `dismiss`, `archive`, the three `bulk-*`, `verify` and `sweep`.
//
// Its own file rather than more of `handlers.t.cpp` (already 3.6k lines)
// because everything here shares one discipline that file does not have:
//
// ## EVERY CASE ASSERTS DATABASE ROWS, NOT ONLY STDOUT
//
// The defect class this cycle exists to close is a verb that exits 0 with
// oracle-identical stdout and wrong rows — `capture session` wrote NULL
// columns that way, and `list_plans` with no status predicate walked
// terminal plans into an aggregate and flipped `done` plans back to
// `active`, green the whole time because no caller yet distinguished
// "empty means open" from "empty means all". A stdout assertion cannot see
// either. So each case here reads the rows back through
// `row_snapshot`/`tag_snapshot`, which render SQL NULL as the literal
// `<NULL>` so it is DISTINGUISHABLE from the empty string — the two are
// different in this schema (`title` and `slug` are nullable, `body`,
// `vendor` and the three `anchor_*` text columns are `not null default ''`)
// and a port that confuses them passes a stdout diff.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, ON A FIXTURE WHERE IT CAN
//
// A filter that returns zero rows on a fixture where the true answer is
// zero rows either way proves nothing: an inert filter and a correct one
// are indistinguishable. So each filter case seeds rows on BOTH sides of
// the predicate and asserts the returned set is the proper subset — and
// the `--plan` / `--task` cases seed real `plans` / `tasks` anchors rather
// than settling for the empty answer, because those columns are foreign
// keys and an unanchored fixture cannot populate them at all.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string below was captured from
// `zig/zig-out/bin/planar` run against a scratch `PLANAR_DB` in a
// registered scratch project, read back through `od -c` (NOT `cat -A`,
// which this platform's BSD `cat` does not support, and not through a
// pipe into `tail`, which hides diagnostics printed before a summary).
// The captures that decided a shape, verbatim:
//
//   $Z annotate show 99      exit 1  stderr b'error: no annotation with id 99\n'
//   $Z annotate tag 99 q     exit 1  stderr b'error: annotate tag: NotFound\n'
//                            ^ the family's odd one out; see below
//   $Z annotate show my-slug exit 2  stderr b"error: annotation id must be an
//                                             integer, got 'my-slug'\n"
//   $Z annotate remove 12    exit 0  stdout b'annotation 12 removed\n'   (22 B)
//   $Z annotate verify --anchor-path zzz.txt
//                            exit 0  stdout b'(no active annotations)\n'  (24 B)
//   $Z annotate verify --anchor-path zzz.txt --json
//                            exit 0  stdout b'{"ok":true,"rows":[]}\n'    (22 B)
//   $Z annotate bulk-resolve --anchor-path zzz.txt
//                            exit 0  stdout b'resolved: 0 annotation(s)\n'(26 B)
//   $Z annotate sweep --since-days 99
//                            exit 0  stdout b'sweep: archived 0 annotation(s)
//                                             older than 99 day(s)\n'    (53 B)
//   $Z annotate resolve <archived>
//                            exit 1  stderr b'error: annotate resolve:
//                                             TerminalStatus\n'
//   $Z annotate update 7 --status bogus
//                            exit 1  stderr b"error: unknown status 'bogus'\n"
//
// The byte COUNTS are quoted because they are what settles the terminator
// question per leaf, and the terminator rule here is genuinely per-renderer
// rather than per-family: `render_verify_text` includes its own newlines
// and `render_verify_json` does not, in the same leaf.
//
// ## THE `tag` / `NotFound` ASYMMETRY IS REAL AND IS PINNED
//
// Six leaves rewrite the engine's `NotFound` into `no annotation with id
// N`; `tag` alone reports `annotate tag: NotFound`. Both were captured
// against the same database with no row 99. Every one of them exits 1, so
// an exit-code comparison sees nothing — normalising them together would
// have been invisible to every gate except a byte assertion, which is why
// there is one here for each shape.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.identity;
import planar.engine.planning;
import planar.json_dom;
import planar.cmd.planar.context;
import planar.cmd.planar.cli_log;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.exit;
import planar.cmd.planar.tree;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int                                           code = 0;        ///< The exit code.
  std::optional<planar::cmd::domain_error_kind> kind;            ///< The classified domain failure, when any.
  std::string                                   out;             ///< Everything written to stdout.
  std::string                                   err;             ///< Everything written to stderr.
  bool                                          db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A scratch root plus the environment and database path every case
/// in this file dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
///
/// Nothing here reads the real environment, so there is no path by which
/// the operator's `~/.planar/planar.db` can be reached — the hazard this
/// cycle's brief names first, and the one a from-source binary run by hand
/// hits by migrating the shared database past every installed binary.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_ann_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "proj" / "sub", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table inside `fx`,
/// from the fixture's project directory.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args, std::optional<std::string_view> stdin_text = std::nullopt)
    -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::istringstream input{stdin_text.value_or("")};
  struct cin_restore {
    std::streambuf* previous;
    ~cin_restore() {
      std::cin.rdbuf(previous);
    }
  } restore{std::cin.rdbuf(input.rdbuf())};
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree    = planar::cmd::root_app();
  auto const         table   = planar::cmd::handlers(*tree);
  auto const         outcome = planar::cmd::run_detailed(ctx, *tree, table);
  return invocation{.code = outcome.code, .kind = outcome.kind, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

/// @brief Dispatch from an arbitrary directory under the fixture root.
///
/// `annotate verify` resolves each anchor path against the OPERATOR CWD,
/// which is only observable by moving the cwd — running the same command
/// from `proj/sub` turns every `fresh` verdict `stale`. Oracle-derived:
/// the same move against the reference binary produced the same flip.
/// @param fx The fixture.
/// @param cwd The directory to run from.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch_in(const fixture& fx, const std::filesystem::path& cwd, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), cwd, fx.db_path, out, err};
  auto const         tree    = planar::cmd::root_app();
  auto const         table   = planar::cmd::handlers(*tree);
  auto const         outcome = planar::cmd::run_detailed(ctx, *tree, table);
  return invocation{.code = outcome.code, .kind = outcome.kind, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

/// @brief Open the fixture's database directly, for row assertions.
///
/// Returns the OWNING connection rather than a borrowed one so a case can
/// snapshot rows after the handler under test has finished and its own
/// context has been destroyed.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief Preserve an on-disk witness byte-for-byte around a read admission.
auto database_bytes(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// @brief Every column of one annotation row, pipe-joined, with SQL NULL
/// rendered as the literal `<NULL>`.
///
/// The `<NULL>` rendering is the point. `title` and `slug` are nullable
/// while `body`, `vendor` and the three `anchor_*` text columns are `not
/// null default ''`, so a port that writes `''` where the oracle writes
/// NULL (or the reverse) produces IDENTICAL stdout — `render_json` emits
/// `""` for one and `null` for the other, but only if the row underneath is
/// right. Collapsing the two here would forfeit the whole assertion.
///
/// `created_at` and `updated_at` are excluded because they are wall-clock
/// and cannot be compared to a literal; the timestamp property that
/// matters is carried instead by the trailing `untouched`/`touched` field,
/// which is `updated_at = created_at` and is what pins the documented
/// no-op-`update` behaviour without depending on the clock.
/// @param conn An open connection to the fixture database.
/// @param id The annotation id.
/// @return The rendered row, or `"<absent>"` when no such row exists.
auto row_snapshot(planar::db::connection& conn, std::int64_t id) -> std::string {
  auto stmt = conn.prepare(R"(select
      scope_kind,
      coalesce(cast(scope_id as text), '<NULL>'),
      anchor_path,
      coalesce(cast(anchor_line_start as text), '<NULL>'),
      coalesce(cast(anchor_line_end as text), '<NULL>'),
      anchor_commit_sha, anchor_text_hash, anchor_text,
      coalesce(title, '<NULL>'),
      coalesce(slug, '<NULL>'),
      body, status, vendor,
      coalesce(cast(plan_id as text), '<NULL>'),
      coalesce(cast(task_id as text), '<NULL>'),
      case when updated_at = created_at then 'untouched' else 'touched' end
    from annotations where id = ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, id).has_value());
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  if (*stepped == planar::db::step_result::done) {
    return "<absent>";
  }
  std::string joined;
  for (int col = 0; col < 16; ++col) {
    if (col > 0) {
      joined += '|';
    }
    joined += stmt->column_text(col);
  }
  return joined;
}

/// @brief The ids in the table, ascending, comma-joined.
///
/// Used by the `remove` cases: "the row is gone" is a claim about the
/// TABLE, and asserting only that `show` now fails would also pass if
/// `remove` had corrupted the row rather than deleted it.
/// @param conn An open connection to the fixture database.
/// @return e.g. `"1,3,4"`, or `""` when the table is empty.
auto id_list(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select id from annotations order by id");
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ',';
    }
    joined += stmt->column_text(0);
  }
  return joined;
}

/// @brief One annotation's tags, ascending, comma-joined.
/// @param conn An open connection to the fixture database.
/// @param id The annotation id.
/// @return e.g. `"a,b"`, or `""` when the row carries no tags.
auto tag_snapshot(planar::db::connection& conn, std::int64_t id) -> std::string {
  auto stmt = conn.prepare("select tag from annotation_tags where annotation_id = ? order by tag");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, id).has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ',';
    }
    joined += stmt->column_text(0);
  }
  return joined;
}

/// @brief The statuses of every row, as `id=status` pairs, ascending.
///
/// The bulk and sweep cases assert on this rather than on the reported
/// count alone. A count is a summary the handler computes; the statuses
/// are what actually shipped, and `bulk_apply` is documented as NOT
/// transactional, so a pass that fails partway leaves a prefix applied and
/// a count that describes it correctly. Only the row states distinguish
/// "transitioned exactly the matching rows" from "transitioned some rows".
/// @param conn An open connection to the fixture database.
/// @return e.g. `"1=active,2=resolved"`.
auto status_map(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select id, status from annotations order by id");
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ',';
    }
    joined += std::format("{}={}", stmt->column_text(0), stmt->column_text(1));
  }
  return joined;
}

/// @brief Write a file under the fixture's project directory.
/// @param fx The fixture.
/// @param relative The path relative to `proj/`.
/// @param content The bytes to write.
auto write_file(const fixture& fx, std::string_view relative, std::string_view content) -> void {
  auto const      full = fx.root / "proj" / std::filesystem::path{relative};
  std::error_code ec;
  std::filesystem::create_directories(full.parent_path(), ec);
  std::ofstream file(full, std::ios::binary);
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// @brief Join the fixture's project directory to an association, so the
/// cwd-derived scope resolves to `association:1` rather than `global`.
///
/// Seeded through `engine_identity`'s own API rather than raw SQL, so the
/// fixture walks the same path `assoc create` / `assoc add` would.
/// @param fx The fixture.
/// @param slug The association slug.
/// @param dir The directory to join.
auto join_association(const fixture& fx, std::string_view slug, const std::filesystem::path& dir) -> void {
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto               conn = ctx.ensure_db();
  REQUIRE(conn.has_value());
  auto const created = planar::engine::identity::create(
      **conn, {.slug = std::string{slug}, .kind = planar::engine::identity::association_kind::project});
  REQUIRE(created.has_value());
  auto const joined = planar::engine::identity::add_member(**conn, slug, dir.string());
  REQUIRE(joined.has_value());
}

/// @brief `annotate add`, asserted to succeed, returning nothing.
/// @param fx The fixture.
/// @param args The `annotate add` argv tail (without `annotate add`).
auto seed(const fixture& fx, std::vector<std::string> args) -> void {
  std::vector<std::string> full{"annotate", "add"};
  full.insert(full.end(), args.begin(), args.end());
  auto const added = dispatch(fx, std::move(full));
  REQUIRE(added.code == 0);
  REQUIRE(added.err.empty());
}

} // namespace

// =========================================================================
// show
// =========================================================================

TEST_CASE("annotate show renders both wire formats and leaves the row alone", "[cmd][annotate][show][rows]") {
  auto const fx = make_fixture("show");
  seed(fx, {"--anchor-path", "f.txt", "--title", "T1", "--body", "B1", "--tags", "a,b"});

  auto       conn   = open_db(fx);
  auto const before = row_snapshot(conn, 1);
  // NULL slug, empty-string body-adjacent columns: the two renderings are
  // different and both appear in this single row.
  CHECK(before == "global|<NULL>|f.txt|<NULL>|<NULL>|||"
                  "|T1|<NULL>|B1|active||<NULL>|<NULL>|untouched");

  auto const text = dispatch(fx, {"annotate", "show", "1"});
  REQUIRE(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.db_open);
  CHECK(text.out.starts_with("id:          1\n"
                             "title:       T1\n"
                             "status:      active\n"
                             "scope:       global\n"
                             "anchor path: f.txt\n"
                             "tags:        a, b\n"
                             "body:        B1\n"
                             "created:     "));
  // `starts_with` alone cannot see a SPURIOUS TRAILING NEWLINE, and a
  // break-probe proved it: appending `'\n'` on the text path — the mistake
  // the `--json` path's contract actively invites, since `render_json` IS
  // a fragment — survived the assertion above untouched. `render_text`
  // carries its own terminators, so this leaf appends nothing, and
  // `emit_one` is shared by SIX leaves, so one stray byte here would
  // diverge all of them at once. Pin the tail and the line count.
  CHECK(text.out.ends_with("Z\n"));
  CHECK_FALSE(text.out.ends_with("\n\n"));
  CHECK(std::ranges::count(text.out, '\n') == 9);

  auto const json = dispatch(fx, {"annotate", "show", "1", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.starts_with(
      "{\"id\":1,\"scope_kind\":\"global\",\"scope_id\":null,"
      "\"anchor\":{\"kind\":\"file\",\"path\":\"f.txt\",\"line_start\":null,\"line_end\":null,"
      "\"commit_sha\":\"\",\"text_hash\":\"\",\"text\":\"\"},"
      "\"target\":null,\"title\":\"T1\",\"slug\":null,\"body\":\"B1\",\"status\":\"active\","
      "\"vendor\":\"\",\"origin\":null,\"revision\":1,\"plan_id\":null,\"task_id\":null,\"tags\":[\"a\",\"b\"],"));
  // render_json is a documented FRAGMENT, so the handler appends exactly
  // one terminator.
  CHECK(json.out.ends_with("\"}\n"));
  CHECK(std::ranges::count(json.out, '\n') == 1);

  // A read verb must not write. `untouched` still holds, which no stdout
  // assertion can express.
  CHECK(row_snapshot(conn, 1) == before);
}

TEST_CASE("annotation read JSON exposes stable entity targets, revisions, filters, and capabilities without a mutation",
          "[cmd][annotate][read][6685]") {
  auto const fx = make_fixture("readcontract");
  REQUIRE(dispatch(fx, {"annotate", "list"}).code == 0);
  auto conn = open_db(fx);
  auto plan = planar::engine::planning::create_plan(conn, {.title = "Read target"});
  REQUIRE(plan.has_value());
  namespace ann     = planar::engine::planning::annotation;
  auto const source = ann::source_uuid(conn);
  REQUIRE(source.has_value());
  auto created = ann::execute_command(conn, {.operation      = ann::command_kind::create,
                                             .operation_uuid = "read-contract-create",
                                             .source_uuid    = *source,
                                             .target         = ann::entity_target{.kind = ann::target_kind::plan, .id = plan->id},
                                             .title          = "Entity note",
                                             .body           = "stable body",
                                             .tags           = {"inbox"}});
  REQUIRE(created.has_value());
  REQUIRE(created->annotation_id.has_value());
  const auto id     = *created->annotation_id;
  const auto before = ann::show(conn, id);
  REQUIRE(before.has_value());

  auto const list = dispatch(fx, {"annotate", "list", "--anchor-kind", "entity", "--target-kind", "plan", "--target-id",
                                  std::to_string(plan->id), "--json"});
  REQUIRE(list.code == 0);
  CHECK(list.out.contains(std::format("\"id\":{}", id)));
  CHECK(list.out.contains(std::format("\"target\":{{\"kind\":\"plan\",\"id\":{}}}", plan->id)));
  CHECK(list.out.contains("\"anchor\":{\"kind\":\"entity\",\"path\":null"));
  CHECK(list.out.contains("\"revision\":1"));

  auto const shown = dispatch(fx, {"annotate", "show", std::to_string(id), "--json"});
  REQUIRE(shown.code == 0);
  CHECK(list.out == "[" + shown.out.substr(0, shown.out.size() - 1) + "]\n");

  auto const capabilities = dispatch(fx, {"annotate", "capabilities", "--json"});
  REQUIRE(capabilities.code == 0);
  CHECK(capabilities.out.contains(std::format("\"source_uuid\":\"{}\"", *source)));
  CHECK(capabilities.out.contains("\"annotation_read\":true"));
  CHECK(capabilities.out.contains("\"entity_anchors\":true"));
  CHECK(capabilities.out.contains("\"filters\":[\"anchor_kind\",\"target_kind\",\"target_id\""));

  auto const after = ann::show(conn, id);
  REQUIRE(after.has_value());
  CHECK(after->revision == before->revision);
  CHECK(after->updated_at == before->updated_at);
}

TEST_CASE("annotation capabilities refuses old and ahead sources without applying migrations",
          "[cmd][annotate][capabilities][readonly][6689]") {
  auto const old = make_fixture("capabilities_old_source");
  {
    auto conn = open_db(old);
    REQUIRE(planar::db::apply_all(conn, planar::db::migrations().subspan(0, 33)).has_value());
  }
  const auto old_before = database_bytes(old.db_path);
  const auto old_result = dispatch(old, {"annotate", "capabilities", "--json"});
  REQUIRE(old_result.code == 0);
  CHECK_FALSE(old_result.db_open);
  CHECK(old_result.out.contains("\"available\":false"));
  CHECK(old_result.out.contains("\"reason\":\"schema_incompatible\""));
  CHECK(old_result.out.contains("\"observed_schema_version\":33"));
  CHECK(database_bytes(old.db_path) == old_before);
  {
    auto       conn  = open_db(old);
    auto const state = planar::db::assert_schema_compatible(conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == 33);
    CHECK(state->verdict_ == planar::db::schema_compatibility::behind);
  }

  auto const ahead = make_fixture("capabilities_ahead_source");
  {
    auto conn = open_db(ahead);
    REQUIRE(planar::db::apply_all(conn).has_value());
    REQUIRE(conn.execute("insert into schema_migrations(version, description) values (999, 'future source')").has_value());
  }
  const auto ahead_before = database_bytes(ahead.db_path);
  const auto ahead_result = dispatch(ahead, {"annotate", "capabilities", "--json"});
  REQUIRE(ahead_result.code == 0);
  CHECK_FALSE(ahead_result.db_open);
  CHECK(ahead_result.out.contains("\"available\":false"));
  CHECK(ahead_result.out.contains("\"observed_schema_version\":999"));
  CHECK(database_bytes(ahead.db_path) == ahead_before);
  {
    auto       conn  = open_db(ahead);
    auto const state = planar::db::assert_schema_compatible(conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == 999);
    CHECK(state->verdict_ == planar::db::schema_compatibility::ahead);
  }
}

TEST_CASE("annotate show reports a missing id by NAMING it, at exit 1", "[cmd][annotate][show][parity]") {
  // Exit 1 and not the not-found bucket: parity-triage §F-exit-code-not-found
  // deliberately folded NotFound back into the generic bucket so scripts can
  // `|| exit 1` cleanly. A distinct code here would look tidier and diverge.
  auto const fx  = make_fixture("shownf");
  auto const got = dispatch(fx, {"annotate", "show", "99"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: no annotation with id 99\n");
}

TEST_CASE("annotate show refuses a non-integer id at exit 2 without opening SQLite", "[cmd][annotate][show][parity]") {
  // The engine offers `show_by_slug` and this leaf deliberately does not
  // reach for it: the oracle answers `annotate show my-slug` with the
  // integer refusal, so accepting a slug would be a capability the
  // reference does not have. `db_open` false is the second half — the
  // refusal happens before any connection, so a fixture with no database
  // still produces it.
  auto const fx  = make_fixture("showslug");
  auto const got = dispatch(fx, {"annotate", "show", "my-slug"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: annotation id must be an integer, got 'my-slug'\n");
  CHECK_FALSE(got.db_open);
}

// =========================================================================
// update
// =========================================================================

TEST_CASE("annotate update writes exactly the patched columns", "[cmd][annotate][update][rows]") {
  auto const fx = make_fixture("update");
  seed(fx, {"--anchor-path", "f.txt", "--line-start", "2", "--line-end", "3", "--commit-sha", "abc123", "--title", "T1", "--body",
            "B1", "--vendor", "vv"});

  auto conn = open_db(fx);
  REQUIRE(row_snapshot(conn, 1) == "global|<NULL>|f.txt|2|3|abc123||"
                                   "|T1|<NULL>|B1|active|vv|<NULL>|<NULL>|untouched");

  auto const got = dispatch(fx, {"annotate", "update", "1", "--title", "T1b", "--body", "B1 revised", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());

  // The anchor columns MUST survive. `annotate update` declares no anchor
  // flags, so the patch passes `anchor = nullopt`; handing the engine a
  // default-constructed `anchor_fields` instead would blank `anchor_path`,
  // the line range and `anchor_commit_sha` on every update, and the JSON
  // on stdout would still be well-formed and plausible.
  CHECK(row_snapshot(conn, 1) == "global|<NULL>|f.txt|2|3|abc123||"
                                 "|T1b|<NULL>|B1 revised|active|vv|<NULL>|<NULL>|touched");
}

TEST_CASE("annotate update with no patch is a no-op that does not bump updated_at", "[cmd][annotate][update][rows]") {
  // The engine's `update` documents this and the oracle confirms it: a bare
  // `annotate update 1` re-renders the row and leaves `updated_at` alone.
  // Only the row can show it — stdout carries a timestamp either way and
  // both readings look correct in isolation.
  auto const fx = make_fixture("updatenoop");
  seed(fx, {"--anchor-path", "f.txt", "--title", "T1"});

  auto conn = open_db(fx);
  REQUIRE(row_snapshot(conn, 1).ends_with("|untouched"));

  auto const got = dispatch(fx, {"annotate", "update", "1"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with("id:          1\n"));
  CHECK(row_snapshot(conn, 1).ends_with("|untouched"));
}

TEST_CASE("annotate update rejects an unknown --status at exit 1 before opening SQLite", "[cmd][annotate][update][parity]") {
  // Same bucket and same wording as `annotate list --status bogus`, and the
  // check runs BEFORE the database opens — which is why the message wins
  // over the not-found one on `annotate update 99 --status bogus`, a shape
  // the oracle was probed for directly.
  auto const fx  = make_fixture("updatestatus");
  auto const got = dispatch(fx, {"annotate", "update", "99", "--status", "bogus"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: unknown status 'bogus'\n");
  CHECK_FALSE(got.db_open);
}

TEST_CASE("annotate update reports a missing id by naming it", "[cmd][annotate][update][parity]") {
  auto const fx  = make_fixture("updatenf");
  auto const got = dispatch(fx, {"annotate", "update", "99", "--title", "x"});
  CHECK(got.code == 1);
  CHECK(got.err == "error: no annotation with id 99\n");
}

// =========================================================================
// remove
// =========================================================================

TEST_CASE("annotate remove deletes the row and cascades its tags", "[cmd][annotate][remove][rows]") {
  auto const fx = make_fixture("remove");
  seed(fx, {"--anchor-path", "f.txt", "--title", "T1", "--tags", "a,b"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "T2", "--tags", "c"});

  auto conn = open_db(fx);
  REQUIRE(id_list(conn) == "1,2");
  REQUIRE(tag_snapshot(conn, 1) == "a,b");

  auto const text = dispatch(fx, {"annotate", "remove", "1", "--expected-revision", "1"});
  REQUIRE(text.code == 0);
  CHECK(text.out == "annotation 1 removed\n");

  // The TABLE, not just `show`: a corrupted-but-present row would still
  // make `show` fail in some ways and is a different bug.
  CHECK(id_list(conn) == "2");
  CHECK(row_snapshot(conn, 1) == "<absent>");
  // `annotation_tags` is `on delete cascade`; the neighbour's tags survive.
  CHECK(tag_snapshot(conn, 1).empty());
  CHECK(tag_snapshot(conn, 2) == "c");

  auto const json = dispatch(fx, {"annotate", "remove", "2", "--expected-revision", "1", "--json"});
  REQUIRE(json.code == 0);
  // Both renderers are fragments, so BOTH paths append — unlike `show`,
  // whose text renderer carries its own newlines.
  CHECK(json.out == "{\"ok\":true,\"id\":2}\n");
  CHECK(id_list(conn).empty());
}

TEST_CASE("annotate remove reports a missing id by naming it", "[cmd][annotate][remove][parity]") {
  auto const fx  = make_fixture("removenf");
  auto const got = dispatch(fx, {"annotate", "remove", "99", "--expected-revision", "1"});
  CHECK(got.code == 1);
  CHECK(got.err == "error: no annotation with id 99\n");
}

// =========================================================================
// tag
// =========================================================================

TEST_CASE("annotate tag adds and removes exactly one tag row", "[cmd][annotate][tag][rows]") {
  auto const fx = make_fixture("tag");
  seed(fx, {"--anchor-path", "f.txt", "--title", "T1", "--tags", "a"});

  auto conn = open_db(fx);
  REQUIRE(tag_snapshot(conn, 1) == "a");

  auto const added = dispatch(fx, {"annotate", "tag", "1", "zed"});
  REQUIRE(added.code == 0);
  CHECK(added.out == "annotation 1: added tag 'zed'\n");
  CHECK(tag_snapshot(conn, 1) == "a,zed");

  auto const json = dispatch(fx, {"annotate", "tag", "1", "zed", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out == "{\"ok\":true,\"id\":1,\"tag\":\"zed\",\"action\":\"add\"}\n");
  // Re-adding is idempotent at the row level — `annotation_tags` is keyed
  // on (annotation_id, tag) — and the handler still reports success.
  CHECK(tag_snapshot(conn, 1) == "a,zed");

  auto const removed = dispatch(fx, {"annotate", "tag", "1", "zed", "--remove", "--json"});
  REQUIRE(removed.code == 0);
  CHECK(removed.out == "{\"ok\":true,\"id\":1,\"tag\":\"zed\",\"action\":\"remove\"}\n");
  CHECK(tag_snapshot(conn, 1) == "a");

  auto const removed_text = dispatch(fx, {"annotate", "tag", "1", "a", "--remove"});
  REQUIRE(removed_text.code == 0);
  CHECK(removed_text.out == "annotation 1: removed tag 'a'\n");
  CHECK(tag_snapshot(conn, 1).empty());

  // `--remove` on a tag the row does not carry succeeds and changes
  // nothing. Asserted because "reports success" and "removed something"
  // are different claims and only the row separates them.
  auto const absent = dispatch(fx, {"annotate", "tag", "1", "never-there", "--remove", "--json"});
  CHECK(absent.code == 0);
  CHECK(tag_snapshot(conn, 1).empty());
}

TEST_CASE("annotate tag is the one leaf that does NOT rewrite NotFound", "[cmd][annotate][tag][parity]") {
  // Its six siblings answer `no annotation with id 99`. Both shapes were
  // captured against the same database; both exit 1, so nothing but a byte
  // assertion distinguishes them. See this file's header.
  auto const fx  = make_fixture("tagnf");
  auto const got = dispatch(fx, {"annotate", "tag", "99", "q"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: annotate tag: NotFound\n");
}

// =========================================================================
// resolve / dismiss / archive
// =========================================================================

TEST_CASE("the lifecycle leaves each write their own status", "[cmd][annotate][lifecycle][rows]") {
  auto const fx = make_fixture("lifecycle");
  seed(fx, {"--anchor-path", "f.txt", "--title", "R"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "D"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "A"});

  auto conn = open_db(fx);
  REQUIRE(status_map(conn) == "1=active,2=active,3=active");

  REQUIRE(dispatch(fx, {"annotate", "resolve", "1", "--json"}).code == 0);
  CHECK(status_map(conn) == "1=resolved,2=active,3=active");

  REQUIRE(dispatch(fx, {"annotate", "dismiss", "2", "--json"}).code == 0);
  CHECK(status_map(conn) == "1=resolved,2=dismissed,3=active");

  REQUIRE(dispatch(fx, {"annotate", "archive", "3", "--json"}).code == 0);
  CHECK(status_map(conn) == "1=resolved,2=dismissed,3=archived");

  // Each leaf must write its OWN status. Three separate handlers wired to
  // the same engine entry point would pass a "row changed" assertion and
  // fail this one.
  auto const shown = dispatch(fx, {"annotate", "show", "2"});
  REQUIRE(shown.code == 0);
  CHECK(shown.out.contains("status:      dismissed\n"));
}

TEST_CASE("resolve refuses a terminal row with TerminalStatus, leaving it unchanged", "[cmd][annotate][lifecycle][rows]") {
  auto const fx = make_fixture("terminal");
  seed(fx, {"--anchor-path", "f.txt", "--title", "T"});
  REQUIRE(dispatch(fx, {"annotate", "archive", "1"}).code == 0);

  auto       conn   = open_db(fx);
  auto const before = row_snapshot(conn, 1);

  auto const got = dispatch(fx, {"annotate", "resolve", "1"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  // NOT the id-naming shape: this is not a NotFound, so it takes the
  // leaf-prefixed form. The two live in the same handler and the branch
  // between them is what this pins.
  CHECK(got.err == "error: annotate resolve: TerminalStatus\n");
  // A refusal that had already written would still print this message.
  CHECK(row_snapshot(conn, 1) == before);
}

TEST_CASE("each lifecycle leaf reports a missing id by naming it", "[cmd][annotate][lifecycle][parity]") {
  auto const fx = make_fixture("lifecyclenf");
  for (auto const* leaf : {"resolve", "dismiss", "archive"}) {
    auto const got = dispatch(fx, {"annotate", leaf, "99"});
    CHECK(got.code == 1);
    CHECK(got.err == "error: no annotation with id 99\n");
  }
}

// =========================================================================
// list filters — every one proven to EXCLUDE
// =========================================================================

TEST_CASE("every annotate list filter excludes non-matching rows", "[cmd][annotate][list][filters]") {
  auto const fx = make_fixture("filters");
  seed(fx, {"--anchor-path", "f.txt", "--title", "A1", "--vendor", "vv", "--tags", "k"});
  seed(fx, {"--anchor-path", "other.txt", "--title", "A2", "--vendor", "vv"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "A3", "--tags", "k"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "A4"});

  // Reads the ids out of the JSON array rather than the text table. The
  // first attempt parsed the text, and the EMPTY case defeated it: an
  // empty listing renders the sentence `(no annotations)`, whose first
  // space-delimited token parses as an "id" and made every negative arm
  // report one row. That is the same shape as the bugs this file guards
  // against, in the test harness — an assertion that looked like it was
  // checking exclusion and was checking nothing.
  auto const ids = [&fx](std::vector<std::string> extra) {
    std::vector<std::string> argv{"annotate", "list"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.emplace_back("--json");
    auto const got = dispatch(fx, std::move(argv));
    REQUIRE(got.code == 0);
    std::string                joined;
    std::string_view           rest{got.out};
    constexpr std::string_view key = "{\"id\":";
    while (true) {
      auto const at = rest.find(key);
      if (at == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(at + key.size());
      auto const end = rest.find(',');
      REQUIRE(end != std::string_view::npos);
      if (!joined.empty()) {
        joined += ',';
      }
      joined += rest.substr(0, end);
      rest.remove_prefix(end);
    }
    return joined;
  };

  // The unfiltered baseline every filtered answer is a PROPER SUBSET of.
  REQUIRE(ids({}) == "1,2,3,4");

  // Each pair partitions the baseline, so neither an inert filter (returns
  // everything) nor an over-eager one (returns nothing) can pass both arms.
  CHECK(ids({"--anchor-path", "f.txt"}) == "1,3,4");
  CHECK(ids({"--anchor-path", "other.txt"}) == "2");
  CHECK(ids({"--anchor-path", "nosuch.txt"}).empty());

  CHECK(ids({"--vendor", "vv"}) == "1,2");
  CHECK(ids({"--vendor", "nosuch"}).empty());

  CHECK(ids({"--tag", "k"}) == "1,3");
  CHECK(ids({"--tag", "nosuch"}).empty());

  // Two filters together must INTERSECT, not union and not last-wins.
  CHECK(ids({"--anchor-path", "f.txt", "--vendor", "vv"}) == "1");

  REQUIRE(dispatch(fx, {"annotate", "resolve", "3"}).code == 0);
  CHECK(ids({"--status", "active"}) == "1,2,4");
  CHECK(ids({"--status", "resolved"}) == "3");
  CHECK(ids({"--status", "dismissed"}).empty());
}

// The name must NOT begin with `--`: `catch_discover_tests` registers the
// TEST_CASE title as the ctest name, ctest passes it back to the binary as
// an argument, and Catch2's own CLI then parses a leading `--plan` as a
// flag and exits with `Unrecognised token: --plan`. It fails ONLY under
// ctest — running the binary with a `[tag]` filter passes — so it survives
// exactly the check most likely to be run while iterating.
TEST_CASE("the plan and task filters run on real anchors, not on an empty answer", "[cmd][annotate][list][filters]") {
  // The rest of the filter coverage could prove `--plan` only by returning
  // zero rows on a fixture where zero was the right answer regardless — the
  // exact shape that lets an inert filter pass. `plan_id` and `task_id` are
  // foreign keys, so proving them requires real anchors.
  auto const fx = make_fixture("planfilter");
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto const plan = planar::engine::planning::create_plan(**conn, {.title = "Anchor plan"});
    REQUIRE(plan.has_value());
    auto const task = planar::engine::planning::create_task(**conn, {.title = "Anchor task", .plan_id = plan->id});
    REQUIRE(task.has_value());
  }

  seed(fx, {"--anchor-path", "f.txt", "--title", "HASPLAN", "--plan", "1"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "HASTASK", "--task", "1"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "HASBOTH", "--plan", "1", "--task", "1"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "HASNEITHER"});

  auto conn = open_db(fx);
  // The anchors really landed — an `add` that silently dropped `--plan`
  // would make every filtered answer below empty and self-consistent.
  REQUIRE(row_snapshot(conn, 1).contains("|HASPLAN|<NULL>||active||1|<NULL>|"));
  REQUIRE(row_snapshot(conn, 2).contains("|HASTASK|<NULL>||active||<NULL>|1|"));
  REQUIRE(row_snapshot(conn, 3).contains("|HASBOTH|<NULL>||active||1|1|"));
  REQUIRE(row_snapshot(conn, 4).contains("|HASNEITHER|<NULL>||active||<NULL>|<NULL>|"));

  auto const listed = [&fx](std::vector<std::string> extra) {
    std::vector<std::string> argv{"annotate", "list"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    argv.emplace_back("--json");
    auto const got = dispatch(fx, std::move(argv));
    REQUIRE(got.code == 0);
    return got.out;
  };

  auto const by_plan = listed({"--plan", "1"});
  CHECK(by_plan.contains("\"title\":\"HASPLAN\""));
  CHECK(by_plan.contains("\"title\":\"HASBOTH\""));
  CHECK_FALSE(by_plan.contains("\"title\":\"HASTASK\""));
  CHECK_FALSE(by_plan.contains("\"title\":\"HASNEITHER\""));

  auto const by_task = listed({"--task", "1"});
  CHECK(by_task.contains("\"title\":\"HASTASK\""));
  CHECK(by_task.contains("\"title\":\"HASBOTH\""));
  CHECK_FALSE(by_task.contains("\"title\":\"HASPLAN\""));

  // The intersection, which is the arm that separates AND from OR.
  auto const both = listed({"--plan", "1", "--task", "1"});
  CHECK(both.contains("\"title\":\"HASBOTH\""));
  CHECK_FALSE(both.contains("\"title\":\"HASPLAN\""));
  CHECK_FALSE(both.contains("\"title\":\"HASTASK\""));

  CHECK(listed({"--plan", "999"}) == "[]\n");
  CHECK(listed({"--task", "999"}) == "[]\n");
}

// =========================================================================
// bulk-resolve / bulk-dismiss / bulk-archive
// =========================================================================

TEST_CASE("bulk-resolve transitions only the matching ACTIVE rows", "[cmd][annotate][bulk][rows]") {
  auto const fx = make_fixture("bulkresolve");
  seed(fx, {"--anchor-path", "f.txt", "--title", "M1", "--tags", "k"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "M2", "--tags", "k"});
  seed(fx, {"--anchor-path", "g.txt", "--title", "N1", "--tags", "k"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "N2"});

  auto conn = open_db(fx);

  // The negative arm FIRST, so a pass that transitions everything cannot
  // hide behind a later positive arm.
  auto const miss = dispatch(fx, {"annotate", "bulk-resolve", "--anchor-path", "nosuch.txt", "--json"});
  REQUIRE(miss.code == 0);
  CHECK(miss.out == "{\"ok\":true,\"action\":\"resolved\",\"count\":0}\n");
  CHECK(status_map(conn) == "1=active,2=active,3=active,4=active");

  auto const hit = dispatch(fx, {"annotate", "bulk-resolve", "--anchor-path", "f.txt", "--tag", "k", "--json"});
  REQUIRE(hit.code == 0);
  CHECK(hit.out == "{\"ok\":true,\"action\":\"resolved\",\"count\":2}\n");
  // The COUNT is what the handler computed; the STATUSES are what shipped.
  // Row 3 carries the tag but the wrong path and row 4 the reverse, so a
  // filter that dropped either term flips this.
  CHECK(status_map(conn) == "1=resolved,2=resolved,3=active,4=active");

  // Idempotent: `status_ = active` excludes the rows it just moved.
  auto const again = dispatch(fx, {"annotate", "bulk-resolve", "--anchor-path", "f.txt", "--tag", "k", "--json"});
  REQUIRE(again.code == 0);
  CHECK(again.out == "{\"ok\":true,\"action\":\"resolved\",\"count\":0}\n");
  CHECK(status_map(conn) == "1=resolved,2=resolved,3=active,4=active");
}

TEST_CASE("bulk-archive takes no status restriction where bulk-resolve does", "[cmd][annotate][bulk][rows]") {
  // The difference between the three leaves is not cosmetic: `resolve` and
  // `dismiss` restrict to `active` and `archive` restricts to nothing,
  // because `resolved`/`dismissed` legally progress to `archived`. Wiring
  // all three identically passes every stdout assertion and silently either
  // over- or under-archives.
  auto const fx = make_fixture("bulkarchive");
  seed(fx, {"--anchor-path", "f.txt", "--title", "A"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "B"});
  auto conn = open_db(fx);

  REQUIRE(dispatch(fx, {"annotate", "resolve", "1"}).code == 0);
  REQUIRE(status_map(conn) == "1=resolved,2=active");

  // `bulk-resolve` now sees only row 2 — proof the `active` restriction is
  // applied at all.
  auto const resolved = dispatch(fx, {"annotate", "bulk-resolve", "--json"});
  REQUIRE(resolved.code == 0);
  CHECK(resolved.out == "{\"ok\":true,\"action\":\"resolved\",\"count\":1}\n");

  // `bulk-archive` now sees BOTH, because it applies no such restriction.
  auto const archived = dispatch(fx, {"annotate", "bulk-archive", "--json"});
  REQUIRE(archived.code == 0);
  CHECK(archived.out == "{\"ok\":true,\"action\":\"archived\",\"count\":2}\n");
  CHECK(status_map(conn) == "1=archived,2=archived");
}

TEST_CASE("bulk-dismiss writes 'dismissed' and renders its own participle", "[cmd][annotate][bulk][rows]") {
  auto const fx = make_fixture("bulkdismiss");
  seed(fx, {"--anchor-path", "f.txt", "--title", "A"});
  auto conn = open_db(fx);

  auto const text = dispatch(fx, {"annotate", "bulk-dismiss"});
  REQUIRE(text.code == 0);
  // render_bulk_text is a fragment, so the handler appends.
  CHECK(text.out == "dismissed: 1 annotation(s)\n");
  CHECK(status_map(conn) == "1=dismissed");
}

TEST_CASE("bulk-resolve --scope resolves the slug and excludes other scopes", "[cmd][annotate][bulk][filters]") {
  auto const fx = make_fixture("bulkscope");
  join_association(fx, "alpha", fx.root / "proj");
  // Written from the joined cwd, so it lands in association 1.
  seed(fx, {"--anchor-path", "f.txt", "--title", "INSCOPE"});
  // Written from a directory belonging to no association, so it lands
  // global — the row the scope filter must NOT touch.
  {
    std::error_code ec;
    std::filesystem::create_directories(fx.root / "outside", ec);
    auto const added = dispatch_in(fx, fx.root / "outside", {"annotate", "add", "--anchor-path", "g.txt", "--title", "OUTSIDE"});
    REQUIRE(added.code == 0);
  }

  auto conn = open_db(fx);
  REQUIRE(row_snapshot(conn, 1).starts_with("association|1|"));
  REQUIRE(row_snapshot(conn, 2).starts_with("global|<NULL>|"));

  auto const scoped = dispatch(fx, {"annotate", "bulk-resolve", "--scope", "alpha", "--json"});
  INFO("stderr: " << scoped.err);
  REQUIRE(scoped.code == 0);
  CHECK(scoped.out == "{\"ok\":true,\"action\":\"resolved\",\"count\":1}\n");
  // The out-of-scope row is UNTOUCHED. A `--scope` that parsed but did not
  // filter would report `count: 2` here and, at exit 0 with plausible
  // output, would have already committed the cross-scope mutation.
  CHECK(status_map(conn) == "1=resolved,2=active");

  auto const unknown = dispatch(fx, {"annotate", "bulk-resolve", "--scope", "nosuch", "--json"});
  CHECK(unknown.code == 1);
  CHECK(unknown.err == "error: annotate bulk-resolve: SlugNotFound\n");
  CHECK(status_map(conn) == "1=resolved,2=active");
}

// =========================================================================
// verify
// =========================================================================

TEST_CASE("annotate verify classifies each anchor against the file on disk", "[cmd][annotate][verify]") {
  auto const fx = make_fixture("verify");
  write_file(fx, "f.txt", "hello world\n");
  // SHA-256 of "hello world\n". The engine compares a PREFIX, so a stored
  // partial hash matches by design; a wrong hash of the same length does
  // not.
  constexpr std::string_view real_hash = "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447";

  seed(fx, {"--anchor-path", "f.txt", "--title", "FRESH", "--text-hash", std::string{real_hash}});
  seed(fx, {"--anchor-path", "f.txt", "--title", "DRIFT", "--text-hash", "deadbeef"});
  seed(fx, {"--anchor-path", "missing.txt", "--title", "STALE", "--text-hash", "abc"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "NOHASH"});

  auto const json = dispatch(fx, {"annotate", "verify", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out == "{\"ok\":true,\"rows\":["
                    "{\"id\":1,\"anchor_path\":\"f.txt\",\"state\":\"fresh\"},"
                    "{\"id\":2,\"anchor_path\":\"f.txt\",\"state\":\"drifted\"},"
                    "{\"id\":3,\"anchor_path\":\"missing.txt\",\"state\":\"stale\"},"
                    // No stored hash means no drift SIGNAL, which the engine
                    // classifies `fresh` rather than `stale` — the two are
                    // easy to swap and both look defensible.
                    "{\"id\":4,\"anchor_path\":\"f.txt\",\"state\":\"fresh\"}]}\n");

  auto const text = dispatch(fx, {"annotate", "verify"});
  REQUIRE(text.code == 0);
  CHECK(text.out == "annotation:1  f.txt  [fresh]\n"
                    "annotation:2  f.txt  [drifted]\n"
                    "annotation:3  missing.txt  [stale]\n"
                    "annotation:4  f.txt  [fresh]\n");

  // Read-only: verify must not write the verdict back.
  auto conn = open_db(fx);
  CHECK(status_map(conn) == "1=active,2=active,3=active,4=active");
}

TEST_CASE("annotate verify resolves anchor paths against the OPERATOR CWD", "[cmd][annotate][verify]") {
  // Oracle-derived rather than assumed: running the same command from a
  // subdirectory of the same project turned every `fresh` verdict `stale`,
  // which only happens if the path is joined to the process cwd rather than
  // to the association root. A port that joined the association root would
  // pass every other case in this file.
  auto const fx = make_fixture("verifycwd");
  write_file(fx, "f.txt", "hello\n");
  seed(fx, {"--anchor-path", "f.txt", "--title", "T"});

  auto const from_root = dispatch(fx, {"annotate", "verify", "--json"});
  REQUIRE(from_root.code == 0);
  CHECK(from_root.out == "{\"ok\":true,\"rows\":[{\"id\":1,\"anchor_path\":\"f.txt\",\"state\":\"fresh\"}]}\n");

  auto const from_sub = dispatch_in(fx, fx.root / "proj" / "sub", {"annotate", "verify", "--json"});
  REQUIRE(from_sub.code == 0);
  CHECK(from_sub.out == "{\"ok\":true,\"rows\":[{\"id\":1,\"anchor_path\":\"f.txt\",\"state\":\"stale\"}]}\n");
}

TEST_CASE("annotate verify covers only ACTIVE rows and filters by anchor path", "[cmd][annotate][verify][filters]") {
  auto const fx = make_fixture("verifyfilter");
  write_file(fx, "f.txt", "hello\n");
  write_file(fx, "g.txt", "hello\n");
  seed(fx, {"--anchor-path", "f.txt", "--title", "A"});
  seed(fx, {"--anchor-path", "g.txt", "--title", "B"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "C"});
  REQUIRE(dispatch(fx, {"annotate", "resolve", "3"}).code == 0);

  // Row 3 is `resolved` and therefore out — the `active` restriction is a
  // filter like any other and gets the same exclusion proof.
  auto const all = dispatch(fx, {"annotate", "verify", "--json"});
  REQUIRE(all.code == 0);
  CHECK(all.out == "{\"ok\":true,\"rows\":["
                   "{\"id\":1,\"anchor_path\":\"f.txt\",\"state\":\"fresh\"},"
                   "{\"id\":2,\"anchor_path\":\"g.txt\",\"state\":\"fresh\"}]}\n");

  auto const filtered = dispatch(fx, {"annotate", "verify", "--anchor-path", "g.txt", "--json"});
  REQUIRE(filtered.code == 0);
  CHECK(filtered.out == "{\"ok\":true,\"rows\":[{\"id\":2,\"anchor_path\":\"g.txt\",\"state\":\"fresh\"}]}\n");

  auto const empty_json = dispatch(fx, {"annotate", "verify", "--anchor-path", "nosuch.txt", "--json"});
  REQUIRE(empty_json.code == 0);
  CHECK(empty_json.out == "{\"ok\":true,\"rows\":[]}\n");

  // The text renderer's empty case is a SENTENCE, not zero bytes.
  auto const empty_text = dispatch(fx, {"annotate", "verify", "--anchor-path", "nosuch.txt"});
  REQUIRE(empty_text.code == 0);
  CHECK(empty_text.out == "(no active annotations)\n");
}

TEST_CASE("annotate verify carries its own SlugNotFound wording", "[cmd][annotate][verify][parity]") {
  // `annotate verify: list failed: SlugNotFound` — the extra `list failed: `
  // segment no sibling has. Captured; not inferred from the pattern.
  auto const fx  = make_fixture("verifyslug");
  auto const got = dispatch(fx, {"annotate", "verify", "--scope", "nosuch", "--json"});
  CHECK(got.code == 1);
  CHECK(got.out == planar::cmd::testsupport::json_error_envelope_line("annotate verify", "generic_failure"));
  CHECK(got.err == "error: annotate verify: list failed: SlugNotFound\n");
}

// =========================================================================
// sweep
// =========================================================================

TEST_CASE("annotate sweep archives resolved rows past the cutoff and no others", "[cmd][annotate][sweep][rows]") {
  // `swept: 0` is also what an INERT sweep reports, so the pass needs both
  // a cutoff that excludes and one that includes, on the same fixture.
  auto const fx = make_fixture("sweep");
  seed(fx, {"--anchor-path", "f.txt", "--title", "RESOLVED"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "DISMISSED"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "STILLACTIVE"});
  REQUIRE(dispatch(fx, {"annotate", "resolve", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "dismiss", "2"}).code == 0);

  auto conn = open_db(fx);
  REQUIRE(status_map(conn) == "1=resolved,2=dismissed,3=active");

  // Default cutoff, and the rows are seconds old: nothing is swept, and the
  // reported `since_days` proves the declared default of 30 arrived.
  auto const defaulted = dispatch(fx, {"annotate", "sweep", "--json"});
  REQUIRE(defaulted.code == 0);
  CHECK(defaulted.out == "{\"ok\":true,\"action\":\"sweep\",\"since_days\":30,\"swept\":0}\n");
  CHECK(status_map(conn) == "1=resolved,2=dismissed,3=active");

  auto const text = dispatch(fx, {"annotate", "sweep"});
  REQUIRE(text.code == 0);
  CHECK(text.out == "sweep: archived 0 annotation(s) older than 30 day(s)\n");

  // Cutoff 0 sweeps everything eligible — and only what is eligible. The
  // `active` row must survive, which is the arm that separates "sweeps the
  // outcome states" from "archives the table".
  auto const swept = dispatch(fx, {"annotate", "sweep", "--since-days", "0", "--json"});
  REQUIRE(swept.code == 0);
  CHECK(swept.out == "{\"ok\":true,\"action\":\"sweep\",\"since_days\":0,\"swept\":2}\n");
  CHECK(status_map(conn) == "1=archived,2=archived,3=active");

  auto const again = dispatch(fx, {"annotate", "sweep", "--since-days", "0", "--json"});
  REQUIRE(again.code == 0);
  CHECK(again.out == "{\"ok\":true,\"action\":\"sweep\",\"since_days\":0,\"swept\":0}\n");
}

TEST_CASE("annotate sweep --scope restricts the blast radius to the named scope", "[cmd][annotate][sweep][scope]") {
  // This case was `[divergence]` through task 6148: `--scope` was declared
  // on the leaf and applied by neither the oracle nor `annotation::sweep`,
  // and the port reproduced that faithfully. Task 6150 fixed it in BOTH
  // trees at once, so the assertions are inverted here and the tag is no
  // longer `[divergence]` — the trees agree again.
  //
  // The arm that matters is the SURVIVAL check, not the count. The broken
  // build reported `swept: 2` for the scoped sweep below, which is an
  // accurate count of what it archived; only asserting that the row the
  // scope does NOT name is still `resolved` separates a scoped sweep from
  // a whole-table one.
  auto const fx = make_fixture("sweepscope");
  join_association(fx, "alpha", fx.root / "proj");
  seed(fx, {"--anchor-path", "f.txt", "--title", "INSCOPE"});
  {
    std::error_code ec;
    std::filesystem::create_directories(fx.root / "outside", ec);
    auto const added = dispatch_in(fx, fx.root / "outside", {"annotate", "add", "--anchor-path", "g.txt", "--title", "OUTSIDE"});
    REQUIRE(added.code == 0);
  }
  REQUIRE(dispatch(fx, {"annotate", "resolve", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "resolve", "2"}).code == 0);

  auto conn = open_db(fx);
  REQUIRE(row_snapshot(conn, 1).starts_with("association|1|"));
  REQUIRE(row_snapshot(conn, 2).starts_with("global|<NULL>|"));

  // An unresolvable slug refuses, like every sibling leaf. Exit 1 with the
  // `<leaf>: SlugNotFound` body `annotate list` uses.
  auto const unknown = dispatch(fx, {"annotate", "sweep", "--scope", "nosuch", "--json"});
  CHECK(unknown.code == 1);
  CHECK(unknown.out == planar::cmd::testsupport::json_error_envelope_line("annotate sweep", "generic_failure"));
  CHECK(unknown.err == "error: annotate sweep: SlugNotFound\n");
  CHECK(status_map(conn) == "1=resolved,2=resolved");

  // A resolvable one restricts. Row 2 is global — the scope does not name
  // it — and MUST survive.
  auto const scoped = dispatch(fx, {"annotate", "sweep", "--since-days", "0", "--scope", "alpha", "--json"});
  REQUIRE(scoped.code == 0);
  CHECK(scoped.out == "{\"ok\":true,\"action\":\"sweep\",\"since_days\":0,\"swept\":1}\n");
  CHECK(status_map(conn) == "1=archived,2=resolved");

  // ...and naming the other scope reaches the survivor, proving the first
  // sweep skipped it for scope reasons and not because it was ineligible.
  auto const global_sweep = dispatch(fx, {"annotate", "sweep", "--since-days", "0", "--scope", "global", "--json"});
  REQUIRE(global_sweep.code == 0);
  CHECK(global_sweep.out == "{\"ok\":true,\"action\":\"sweep\",\"since_days\":0,\"swept\":1}\n");
  CHECK(status_map(conn) == "1=archived,2=archived");
}

TEST_CASE("annotate sweep without --scope still spans every scope", "[cmd][annotate][sweep][scope]") {
  // The unscoped sweep is unchanged by 6150 and stays whole-database. This
  // is the arm that would catch a fix that turned the absent flag into an
  // implicit cwd-derived scope.
  auto const fx = make_fixture("sweepnoscope");
  join_association(fx, "alpha", fx.root / "proj");
  seed(fx, {"--anchor-path", "f.txt", "--title", "INSCOPE"});
  {
    std::error_code ec;
    std::filesystem::create_directories(fx.root / "outside", ec);
    auto const added = dispatch_in(fx, fx.root / "outside", {"annotate", "add", "--anchor-path", "g.txt", "--title", "OUTSIDE"});
    REQUIRE(added.code == 0);
  }
  REQUIRE(dispatch(fx, {"annotate", "resolve", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "resolve", "2"}).code == 0);

  auto       conn  = open_db(fx);
  auto const swept = dispatch(fx, {"annotate", "sweep", "--since-days", "0", "--json"});
  REQUIRE(swept.code == 0);
  CHECK(swept.out == "{\"ok\":true,\"action\":\"sweep\",\"since_days\":0,\"swept\":2}\n");
  CHECK(status_map(conn) == "1=archived,2=archived");
}

// =========================================================================
// audit_log rows (plan 1001, task 6100)
// =========================================================================

namespace {

/// @brief Every `audit_log` row, pipe-joined, one per line, SQL NULL as
/// the literal `<NULL>`. Dumps the WHOLE table on purpose -- a missing
/// audit row moves no exit code, no stdout byte and no rendered field, so
/// only a full transcript discriminates. See handlers.t.cpp's twin.
/// @param fx The fixture.
/// @return The transcript, newline-terminated per row.
auto audit_transcript(const fixture& fx) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select verb, entity_kind, entity_id, coalesce(summary, '<NULL>') "
                            "from audit_log order by id");
  REQUIRE(stmt.has_value());
  std::string out;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    out += std::format("{}|{}|{}|{}\n", stmt->column_text(0), stmt->column_text(1), stmt->column_int64(2), stmt->column_text(3));
  }
  return out;
}

} // namespace

// ORACLE PROVENANCE: this exact argv sequence was run against the Zig
// binary on an isolated scratch database and `audit_log` dumped with the
// same projection. The transcript below is that dump, transcribed.
TEST_CASE("the annotate leaves write the oracle's audit_log rows", "[cmd][annotate][audit][6100]") {
  auto const fx = make_fixture("annaudit");
  seed(fx, {"--anchor-path", "f.txt", "--title", "A1"});
  seed(fx, {"--anchor-path", "g.txt"}); // no title
  REQUIRE(dispatch(fx, {"annotate", "update", "1", "--body", "b"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "update", "1", "--status", "resolved"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "dismiss", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "archive", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "tag", "1", "t1"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "tag", "1", "t1", "--remove"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "remove", "2", "--expected-revision", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"annotate", "update", "1"}).code == 0); // no-op patch

  CHECK(audit_transcript(fx) == "create|annotation|1|create annotation 'A1'\n"
                                // An UNTITLED row falls back to the anchor path, not to `<NULL>`
                                // and not to the id.
                                "create|annotation|2|create annotation 'g.txt'\n"
                                "update|annotation|1|<NULL>\n"
                                // `update --status` is a status_change with a NULL summary; the
                                // verb-labelled summaries below come only from the dedicated
                                // transition verbs, and they are the VERB (`dismiss`, `archive`),
                                // one letter off the resulting status (`dismissed`, `archived`).
                                "status_change|annotation|1|<NULL>\n"
                                "status_change|annotation|2|dismiss\n"
                                "status_change|annotation|1|archive\n"
                                "delete|annotation|2|<NULL>\n");
  // Nothing above came from `annotate tag`, `annotate tag --remove`, or
  // the no-op `annotate update 1`. All three exit 0 and all three write
  // NO row -- which is why they are in the sequence at all.
}

// =========================================================================
// Receipt-backed structured annotation commands (task 6684)
// =========================================================================

TEST_CASE("annotation commands commit mutations, revisions, audit rows, and durable receipts together",
          "[cmd][annotate][command][receipt][6684]") {
  auto const fx = make_fixture("commandreceipt");
  // Open through the command context once so this direct engine case uses
  // the same migrated schema as a structured command invocation.
  auto const initialized = dispatch(fx, {"annotate", "list"});
  REQUIRE(initialized.code == 0);
  auto conn = open_db(fx);

  auto const plan = planar::engine::planning::create_plan(conn, {.title = "Command target"});
  REQUIRE(plan.has_value());
  auto const source = planar::engine::planning::annotation::source_uuid(conn);
  REQUIRE(source.has_value());

  namespace ann = planar::engine::planning::annotation;
  ann::command_args create_command{.operation      = ann::command_kind::create,
                                   .operation_uuid = "create-6684",
                                   .source_uuid    = *source,
                                   .target         = ann::entity_target{.kind = ann::target_kind::plan, .id = plan->id},
                                   .title          = "receipt body",
                                   .body           = "v1",
                                   .tags           = {"first"}};
  auto const        created = ann::execute_command(conn, create_command);
  REQUIRE(created.has_value());
  CHECK_FALSE(created->replayed);
  REQUIRE(created->annotation_id.has_value());
  CHECK(created->revision == 1);
  auto const annotation_id = *created->annotation_id;
  CHECK(tag_snapshot(conn, annotation_id) == "first");

  // The same UUID and payload is a receipt replay, not a second mutation.
  auto const replayed = ann::execute_command(conn, create_command);
  REQUIRE(replayed.has_value());
  CHECK(replayed->replayed);
  CHECK(replayed->annotation_id == created->annotation_id);
  CHECK(id_list(conn) == std::to_string(annotation_id));

  auto different_payload = create_command;
  different_payload.body = "different";
  auto const conflict    = ann::execute_command(conn, different_payload);
  REQUIRE_FALSE(conflict.has_value());
  CHECK(conflict.error() == ann::annotation_error::receipt_conflict);

  ann::command_args replace_tags{.operation         = ann::command_kind::replace_tags,
                                 .operation_uuid    = "tags-6684",
                                 .source_uuid       = *source,
                                 .annotation_id     = annotation_id,
                                 .expected_revision = 1,
                                 .tags              = {"alpha", "beta"}};
  auto const        tagged = ann::execute_command(conn, replace_tags);
  REQUIRE(tagged.has_value());
  CHECK_FALSE(tagged->replayed);
  CHECK(tagged->revision == 2);
  CHECK(tag_snapshot(conn, annotation_id) == "alpha,beta");

  ann::command_args resolve_command{.operation         = ann::command_kind::resolve,
                                    .operation_uuid    = "resolve-6684",
                                    .source_uuid       = *source,
                                    .annotation_id     = annotation_id,
                                    .expected_revision = 2};
  auto const        resolved = ann::execute_command(conn, resolve_command);
  REQUIRE(resolved.has_value());
  CHECK(resolved->revision == 3);
  CHECK(row_snapshot(conn, annotation_id).contains("|resolved|"));

  // A stale optimistic-lock request writes neither an audit row nor a receipt.
  ann::command_args stale_edit{.operation         = ann::command_kind::edit,
                               .operation_uuid    = "stale-6684",
                               .source_uuid       = *source,
                               .annotation_id     = annotation_id,
                               .expected_revision = 2,
                               .body              = "must not land"};
  auto const        stale = ann::execute_command(conn, stale_edit);
  REQUIRE_FALSE(stale.has_value());
  CHECK(stale.error() == ann::annotation_error::revision_conflict);
  auto receipt_count = conn.prepare("select count(*) from annotation_operation_receipts");
  REQUIRE(receipt_count.has_value());
  REQUIRE(receipt_count->step().has_value());
  CHECK(receipt_count->column_int64(0) == 3);
  CHECK(audit_transcript(fx).contains("create|annotation|1|create annotation 'receipt body'\n"));
  CHECK(audit_transcript(fx).contains("update|annotation|1|<NULL>\n"));
  CHECK(audit_transcript(fx).contains("status_change|annotation|1|resolve\n"));

  // Lookup is the uncertain-outcome path: it is read-only and preserves the
  // original revision/outcome after the command process has exited.
  auto const looked_up = ann::show_receipt(conn, *source, "resolve-6684");
  REQUIRE(looked_up.has_value());
  REQUIRE(looked_up->has_value());
  CHECK((**looked_up).outcome == "resolve");
  CHECK((**looked_up).revision == 3);
  CHECK((**looked_up).replayed);
}

TEST_CASE("annotation command payload identity is durable and preserves null titles", "[cmd][annotate][command][receipt][6684]") {
  auto const fx = make_fixture("commandidentity");
  REQUIRE(dispatch(fx, {"annotate", "list"}).code == 0);
  auto       conn = open_db(fx);
  auto const plan = planar::engine::planning::create_plan(conn, {.title = "Target"});
  REQUIRE(plan.has_value());
  namespace ann     = planar::engine::planning::annotation;
  auto const source = ann::source_uuid(conn);
  REQUIRE(source.has_value());
  ann::command_args first{.operation      = ann::command_kind::create,
                          .operation_uuid = "delimiter",
                          .source_uuid    = *source,
                          .target         = ann::entity_target{.kind = ann::target_kind::plan, .id = plan->id},
                          .title          = "a|b",
                          .body           = "c"};
  REQUIRE(ann::execute_command(conn, first).has_value());
  auto distinct       = first;
  distinct.title      = "a";
  distinct.body       = "b|c";
  auto const conflict = ann::execute_command(conn, distinct);
  REQUIRE_FALSE(conflict.has_value());
  CHECK(conflict.error() == ann::annotation_error::receipt_conflict);

  ann::command_args edit{.operation         = ann::command_kind::edit,
                         .operation_uuid    = "clear-title",
                         .source_uuid       = *source,
                         .annotation_id     = 1,
                         .expected_revision = 1,
                         .clear_title       = true};
  REQUIRE(ann::execute_command(conn, edit).has_value());
  auto cleared = ann::show(conn, 1);
  REQUIRE(cleared.has_value());
  CHECK_FALSE(cleared->title.has_value());
}

TEST_CASE("annotate remove requires the current revision", "[cmd][annotate][remove][revision][6684]") {
  auto const fx = make_fixture("removerevision");
  seed(fx, {"--anchor-path", "f.txt", "--title", "remove"});
  auto const stale = dispatch(fx, {"annotate", "remove", "1", "--expected-revision", "2"});
  CHECK(stale.code == 1);
  CHECK(stale.err == "error: annotate remove: Conflict\n");
  auto conn = open_db(fx);
  CHECK(id_list(conn) == "1");
  auto const missing = dispatch(fx, {"annotate", "remove", "1"});
  CHECK(missing.code == 2);
  CHECK(missing.err == "error: --expected-revision is required\n");
}

TEST_CASE("bulk command receipts retain their affected count on replay", "[cmd][annotate][bulk][receipt][6684]") {
  auto const fx = make_fixture("bulkreceipt");
  seed(fx, {"--anchor-path", "f.txt", "--title", "one"});
  seed(fx, {"--anchor-path", "f.txt", "--title", "two"});
  REQUIRE(dispatch(fx, {"annotate", "list"}).code == 0);
  auto conn     = open_db(fx);
  namespace ann = planar::engine::planning::annotation;
  auto source   = ann::source_uuid(conn);
  REQUIRE(source.has_value());
  ann::command_args command{.operation      = ann::command_kind::bulk_resolve,
                            .operation_uuid = "bulk-6684",
                            .source_uuid    = *source,
                            .bulk_filter    = ann::list_filter{.anchor_path = "f.txt", .status_ = ann::status::active}};
  auto              first = ann::execute_command(conn, command);
  REQUIRE(first.has_value());
  CHECK(first->affected_count == 2);
  auto replay = ann::execute_command(conn, command);
  REQUIRE(replay.has_value());
  CHECK(replay->replayed);
  CHECK(replay->affected_count == 2);
}

TEST_CASE("annotation command validates its JSON boundary before mutation and emits parseable receipts",
          "[cmd][annotate][command][boundary][6684]") {
  auto const fx = make_fixture("commandboundary");
  REQUIRE(dispatch(fx, {"annotate", "list"}).code == 0);
  auto       conn = open_db(fx);
  auto const plan = planar::engine::planning::create_plan(conn, {.title = "Target"});
  REQUIRE(plan.has_value());
  namespace ann     = planar::engine::planning::annotation;
  auto const source = ann::source_uuid(conn);
  REQUIRE(source.has_value());

  auto const malformed = dispatch(
      fx, {"annotate", "command", "--request", "@-"},
      std::format(
          R"({{"operation":"create","operation_id":"bad-body","source_uuid":"{}","target_kind":"plan","target_id":{},"body":123}})",
          *source, plan->id));
  CHECK(malformed.code == 2);
  CHECK(malformed.out.empty());
  CHECK(malformed.err == "error: body must be a string\n");
  CHECK_FALSE(malformed.db_open);
  auto annotation_count = conn.prepare("select count(*) from annotations");
  auto receipt_count    = conn.prepare("select count(*) from annotation_operation_receipts");
  auto audit_count      = conn.prepare("select count(*) from audit_log");
  REQUIRE(annotation_count.has_value());
  REQUIRE(receipt_count.has_value());
  REQUIRE(audit_count.has_value());
  REQUIRE(annotation_count->step().has_value());
  REQUIRE(receipt_count->step().has_value());
  REQUIRE(audit_count->step().has_value());
  CHECK(annotation_count->column_int64(0) == 0);
  CHECK(receipt_count->column_int64(0) == 0);
  auto const audits_before = audit_count->column_int64(0);
  REQUIRE(annotation_count->reset().has_value());
  REQUIRE(receipt_count->reset().has_value());
  REQUIRE(audit_count->reset().has_value());

  // Cross the informational retention threshold without changing the command
  // under test. The next receipt must remain one complete JSON object.
  auto seeded = conn.prepare("insert into annotation_operation_receipts(operation_uuid, source_uuid, payload_digest, outcome) "
                             "values (?, ?, 'seed', 'create')");
  REQUIRE(seeded.has_value());
  for (int i = 0; i < 10000; ++i) {
    REQUIRE(seeded->bind_text(1, std::format("seed-{}", i)).has_value());
    REQUIRE(seeded->bind_text(2, *source).has_value());
    REQUIRE(seeded->step().has_value());
    REQUIRE(seeded->reset().has_value());
  }

  auto const quoted = dispatch(
      fx, {"annotate", "command", "--request", "@-"},
      std::format(
          R"({{"operation":"create","operation_id":"quote\"id","source_uuid":"{}","target_kind":"plan","target_id":{},"title":"quoted"}})",
          *source, plan->id));
  REQUIRE(quoted.code == 0);
  CHECK(quoted.err.empty());
  auto const response = planar::json_dom::parse_json(quoted.out);
  REQUIRE(response.has_value());
  REQUIRE(response->kind == planar::json_dom::json_kind::object);
  REQUIRE(response->find("operation_uuid") != nullptr);
  CHECK(response->find("operation_uuid")->string == "quote\"id");
  REQUIRE(response->find("retention_warning") != nullptr);
  CHECK(response->find("retention_warning")->kind == planar::json_dom::json_kind::object);

  auto const looked_up = dispatch(fx, {"annotate", "receipt", "--source-uuid", *source, "--operation-id", "quote\"id"});
  REQUIRE(looked_up.code == 0);
  auto const lookup_json = planar::json_dom::parse_json(looked_up.out);
  REQUIRE(lookup_json.has_value());
  REQUIRE(lookup_json->find("found") != nullptr);
  CHECK(lookup_json->find("found")->boolean);
  REQUIRE(lookup_json->find("operation_uuid") != nullptr);
  CHECK(lookup_json->find("operation_uuid")->string == "quote\"id");
  REQUIRE(lookup_json->find("source_uuid") != nullptr);
  CHECK(lookup_json->find("source_uuid")->string == *source);
  REQUIRE(lookup_json->find("outcome") != nullptr);
  CHECK(lookup_json->find("outcome")->string == "create");
  REQUIRE(lookup_json->find("replayed") != nullptr);
  CHECK(lookup_json->find("replayed")->boolean);

  auto after_audit = conn.prepare("select count(*) from audit_log");
  REQUIRE(after_audit.has_value());
  REQUIRE(after_audit->step().has_value());
  CHECK(after_audit->column_int64(0) == audits_before + 1);
}

TEST_CASE("annotation command reports a competing write lock as retryable busy without a receipt",
          "[cmd][annotate][command][busy][6684]") {
  auto const fx = make_fixture("commandbusy");
  REQUIRE(dispatch(fx, {"annotate", "list"}).code == 0);
  auto       conn = open_db(fx);
  auto const plan = planar::engine::planning::create_plan(conn, {.title = "Target"});
  REQUIRE(plan.has_value());
  namespace ann     = planar::engine::planning::annotation;
  auto const source = ann::source_uuid(conn);
  REQUIRE(source.has_value());

  auto lock = conn.begin_transaction(planar::db::lock_mode::immediate);
  REQUIRE(lock.has_value());
  auto const request = std::format(
      R"({{"operation":"create","operation_id":"busy-6684","source_uuid":"{}","target_kind":"plan","target_id":{},"title":"retry"}})",
      *source, plan->id);
  auto const busy = dispatch(fx, {"annotate", "command", "--request", "@-"}, request);
  CHECK(busy.code == 1);
  CHECK(busy.out.empty());
  CHECK(busy.err == "error: annotate command: Busy\n");

  {
    auto annotation_count = conn.prepare("select count(*) from annotations");
    auto receipt_count    = conn.prepare("select count(*) from annotation_operation_receipts");
    auto audit_count      = conn.prepare("select count(*) from audit_log");
    REQUIRE(annotation_count.has_value());
    REQUIRE(receipt_count.has_value());
    REQUIRE(audit_count.has_value());
    REQUIRE(annotation_count->step().has_value());
    REQUIRE(receipt_count->step().has_value());
    REQUIRE(audit_count->step().has_value());
    CHECK(annotation_count->column_int64(0) == 0);
    CHECK(receipt_count->column_int64(0) == 0);
    CHECK(audit_count->column_int64(0) == 1); // create_plan only
  }
  REQUIRE(lock->commit().has_value());

  // main records after command dispatch.  The command's own transaction was
  // deliberately contended above, so release the competing writer before
  // recording its diagnostic on the separate best-effort log connection.
  // This proves the actual command outcome reaches the durable retryable
  // category when persistence is possible; it neither turns Busy into
  // success nor relies on logging while the source remains locked.
  std::filesystem::create_directories(fx.root / "fakehome" / ".planar");
  {
    std::ofstream config(fx.root / "fakehome" / ".planar" / "config.toml");
    config << "[introspection]\ncli_log = true\n";
  }
  std::ostringstream logged_out;
  std::ostringstream logged_err;
  context            logged_ctx{{"planar", "annotate", "command", "--request", "@-"},
                                planar::cmd::map_env(fx.vars),
                                fx.root / "proj",
                                fx.db_path,
                                logged_out,
                                logged_err};
  REQUIRE(busy.kind == planar::cmd::domain_error_kind::busy_source);
  planar::cmd::record(logged_ctx, busy.code, busy.kind, std::chrono::milliseconds{1});
  {
    auto diagnostic = conn.prepare("select verb_path, exit_code, error_category from cli_invocations order by id desc limit 1");
    REQUIRE(diagnostic.has_value());
    REQUIRE(diagnostic->step().has_value());
    CHECK(diagnostic->column_text(0) == "annotate command");
    CHECK(diagnostic->column_int64(1) == 1);
    CHECK(diagnostic->column_text(2) == "busy");
  }

  auto const retried = dispatch(fx, {"annotate", "command", "--request", "@-"}, request);
  REQUIRE(retried.code == 0);
  auto const retry_json = planar::json_dom::parse_json(retried.out);
  REQUIRE(retry_json.has_value());
  REQUIRE(retry_json->find("replayed") != nullptr);
  CHECK_FALSE(retry_json->find("replayed")->boolean);
  CHECK(ann::show_receipt(conn, *source, "busy-6684")->has_value());
}

TEST_CASE("annotation command measures tag limits in UTF-8 characters", "[cmd][annotate][command][unicode][6684]") {
  auto const fx = make_fixture("commandunicode");
  REQUIRE(dispatch(fx, {"annotate", "list"}).code == 0);
  auto       conn = open_db(fx);
  auto const plan = planar::engine::planning::create_plan(conn, {.title = "Target"});
  REQUIRE(plan.has_value());
  namespace ann     = planar::engine::planning::annotation;
  auto const source = ann::source_uuid(conn);
  REQUIRE(source.has_value());

  // Constructing by concatenation preserves the UTF-8 bytes while making the
  // 63/64/65 code-point boundaries explicit.
  auto tags_request = [&](std::string_view operation_id, std::string_view tag, std::optional<std::int64_t> annotation_id,
                          std::optional<std::int64_t> revision) {
    std::string request = std::format("{{\"operation\":\"{}\",\"operation_id\":\"{}\",\"source_uuid\":\"{}\",\"tags\":[\"{}\"]",
                                      annotation_id ? "replace-tags" : "create", operation_id, *source, tag);
    if (annotation_id)
      request += std::format(",\"annotation_id\":{},\"expected_revision\":{}", *annotation_id, *revision);
    else
      request += std::format(",\"target_kind\":\"plan\",\"target_id\":{}", plan->id);
    return request + '}';
  };
  auto const tag63 = std::string(62, 'x') + "é"; // 63 code points, 64 bytes.
  auto const first = dispatch(fx, {"annotate", "command", "--request", "@-"}, tags_request("unicode-63", tag63, {}, {}));
  REQUIRE(first.code == 0);
  auto const tag64  = std::string(62, 'x') + "éé"; // 64 code points, 66 bytes.
  auto const second = dispatch(fx, {"annotate", "command", "--request", "@-"}, tags_request("unicode-64", tag64, 1, 1));
  REQUIRE(second.code == 0);
  auto const tag65    = std::string(63, 'x') + "éé"; // 65 code points.
  auto const rejected = dispatch(fx, {"annotate", "command", "--request", "@-"}, tags_request("unicode-65", tag65, 1, 2));
  CHECK(rejected.code == 2);
  CHECK(rejected.err == "error: tags must contain strings of at most 64 characters\n");
  CHECK(tag_snapshot(conn, 1) == tag64);
}
