// @file links_leaves.t.cpp
// @brief In-process tests for the seven leaves wired by plan 996, task
// 6193 — the whole entity-link surface: `links add`, `links list`, `links
// remove`, `links trail`, `plan link`, `task link` and `question link`.
//
// Its own file rather than more of `handlers.t.cpp`, on the discipline
// `annotate_leaves.t.cpp`, `question_leaves.t.cpp` and
// `plan_task_remainder_leaves.t.cpp` established.
//
// ## EVERY CASE ASSERTS DATABASE ROWS
//
// Stdout is checked where the bytes are the contract, but no case rests on
// stdout alone. Two tables are in play on every mutation — `entity_links`
// and `audit_log` — and the audit half was UNWIRED in this bucket until
// this task, having passed every engine unit test for four cycles because
// no reader existed. So every mutation and every REFUSAL is followed by a
// snapshot of both.
//
// SQL NULL renders as the literal `<NULL>` and is never collapsed onto the
// empty string. `audit_log.actor`, `.scope` and `.summary` are all
// genuinely nullable and all three are NULL on this path: they render as
// `null` in `links trail --json` and as `(none)` in its text form, so a
// port that wrote `''` would be operator-visible and would still pass
// every verb/entity_id assertion.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, BY A SURVIVOR
//
// Two filters exist on this surface and each gets a MULTI-WAY
// discrimination rather than a count:
//
//   `links list <ref>`  — an OR over BOTH endpoint columns. The fixture
//       seeds edges among five distinct subjects so that an inert filter
//       returns the same set for every subject, a uniformly-applied one
//       returns empty, and only a correctly-branching one yields several
//       DIFFERENT non-empty answers. Excluded edges are then asserted
//       still present in `entity_links`.
//
//   `links trail <id>` — `entity_kind = 'entity_link' AND entity_id = ?`,
//       a two-column predicate. The fixture seeds a decoy audit row with
//       the SAME entity_id under a DIFFERENT entity_kind and other rows
//       with the same kind under different ids, so dropping EITHER column
//       from the predicate produces a visibly wrong answer. Both decoys
//       are asserted to SURVIVE in `audit_log`.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string was captured by RUNNING `zig/zig-out/bin/
// planar` against scratch arenas, read from files rather than through a
// pipe. The whole set was then re-derived as a SEQUENCE diff: a 49-step
// argv script plus a 23-step one replayed against both binaries with
// stdout, stderr, exit codes AND the resulting `entity_links` /
// `audit_log` dumps compared. The captures that decided a shape:
//
//   $Z links list task:1        exit 0  b'no links for task:1\n'
//   $Z links list task:1 --json exit 0  ZERO BYTES
//       ^ the two empty cases disagree, and the JSON one is not `[]`.
//   $Z links list task:1        b'id     direction  relationship     peer\n'
//                               b'+2     from       depends-on       task:2\n'
//       ^ the id column carries a FORCED `+` SIGN, left-aligned in width
//         6, OVERFLOWING past five digits rather than truncating
//         (`+1234567 from ...`, verified by seeding a high id).
//   $Z links add a b --rel r    b'created entity_link: task:1 --[cites]--> task:2  (link id: 10)\n'
//   $Z plan link 1 task:3 --rel r b'linked plan:1 -> task:3  [cites]  (link id: 4)\n'
//       ^ TWO different success shapes for one insert, and both use DOUBLE
//         spaces, in different places.
//   $Z links add   --json  {"ok":true,"id":3,"from_kind":...}   NO created_at
//   $Z links list  --json  {"id":2,"from_kind":...,"created_at":...}  NO ok
//   $Z plan link   --json  {"ok":true,"id":3,"plan_id":1,"to_kind":...}
//       ^ THREE envelopes over one table.
//   $Z links trail 2            b'id     verb         actor        recorded_at\n'
//                               b'+13    link         (none)       2026-...Z\n'
//       ^ `(none)` is the literal for a NULL actor.
//   $Z links trail 999          exit 1  b'error: entity link 999 not found\n'
//   $Z links trail 2 (no rows)  exit 0  b'no audit trail for entity_link:2\n'
//       ^ missing LINK and empty TRAIL are different answers with
//         different exit codes.
//   $Z plan link 1 task:1 --rel depends-on (twice)
//       exit 1  b'error: link plan:1 \xe2\x86\x92 task:1 [depends-on] already exists\n'
//   $Z task link 1 plan:2 --rel derives-from (twice)
//       exit 1  b'error: link task:1 -> plan:2 [derives-from] already exists\n'
//       ^ `plan link` uses U+2192; its two siblings and `links add` use
//         the ASCII `->`. An oracle inconsistency, reproduced.
//   $Z links add ... (duplicate) exit 1, NOT exit 6
//       ^ the precondition-conflict bucket would be 6 and is wrong here.
//   $Z plan link 1 task:4 --rel cites --scope nosuchscope  exit 0, LINK WRITTEN
//       ^ `--scope` is accepted and completely inert; link verbs are
//         unguarded by design.
//   $Z links add task:1 task:2 --relationship blocks  exit 2
//       ^ the pre-migration-00033 spelling is REFUSED, not aliased.
//
// ## The ONE known divergence, and why it is not closed here
//
// A flag given with NO VALUE reports `error: --relationship: 1 required
// missing` / `ArgumentMismatch` where the oracle reports `error: flag
// missing value: --relationship` / `MissingValue`. That is CLI11's
// message, not this surface's: the same diff appears on `task update 1
// --status`, `plan create X --slug` and `question add Q --body`, all of
// which were ported cycles ago. It belongs to the parser layer and is
// pinned here as a KNOWN divergence rather than worked around.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment and database path every case
/// dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
///
/// Nothing here reads the real environment, so there is no path by which
/// the operator's `~/.planar/planar.db` can be reached.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_lk_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Dispatch `args` against the real tree and table inside `fx`.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Open the fixture's database directly, for row assertions.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief Run one read-only query and render its rows as `a|b|c` lines
/// joined by `;`, with SQL NULL as the literal `<NULL>`.
/// @param conn An open connection to the fixture database.
/// @param sql The query. A test-local literal, never operator input.
/// @param columns How many columns the projection selects.
/// @return The rendered rows, or `""` when the query matched nothing.
auto query_rows(planar::db::connection& conn, std::string_view sql, int columns) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ';';
    }
    for (int col = 0; col < columns; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every `entity_links` row as
/// `id|from_kind|from_id|to_kind|to_id|relationship`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto link_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn, "select id, from_kind, from_id, to_kind, to_id, relationship from entity_links order by id", 6);
}

/// @brief The `audit_log` rows for `entity_link`, as
/// `verb|entity_id|summary|actor|scope`.
///
/// `actor`, `scope` and `summary` are included precisely because they are
/// ALWAYS NULL on this path: a port that helpfully filled them in would be
/// writing rows the oracle does not, invisibly to every stdout comparison.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto link_audit_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select verb, entity_id, summary, actor, scope from audit_log "
                    "where entity_kind = 'entity_link' order by id",
                    5);
}

/// @brief Every `audit_log` row as `entity_kind|entity_id|verb`, for the
/// survivor assertions that prove `trail`'s predicate excludes.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto all_audit_kinds(planar::db::connection& conn) -> std::string {
  return query_rows(conn, "select entity_kind, entity_id, verb from audit_log order by id", 3);
}

/// @brief Bring a fixture up to an initialised database holding two plans,
/// eight tasks and one question.
///
/// Seeded THROUGH THE CLI, never by raw SQL, so the fixture exercises the
/// same code paths an operator would and the ids are the oracle's.
/// @param fx The fixture.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Demo", "--slug", "demo-plan", "--summary", "S", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Two", "--slug", "two-plan", "--summary", "S", "--scope", "global"}).code == 0);
  for (int i = 1; i <= 8; ++i) {
    REQUIRE(dispatch(fx, {"task", "add", std::format("T{}", i), "--plan", "1", "--editor=false", "--scope", "global"}).code == 0);
  }
  REQUIRE(dispatch(fx, {"question", "add", "Q1?", "--scope", "global"}).code == 0);
}

} // namespace

// ===========================================================================
// links add
// ===========================================================================

TEST_CASE("links add writes the edge AND its audit row, and echoes the oracle's line") {
  auto const fx = make_fixture("add");
  seed(fx);

  auto const added = dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "depends-on"});
  CHECK(added.code == 0);
  CHECK(added.err.empty());
  // DOUBLE space before `(link id:`.
  CHECK(added.out == "created entity_link: task:1 --[depends-on]--> task:2  (link id: 1)\n");

  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|task|1|task|2|depends-on");
  // The audit half. This bucket wrote NO audit row until task 6193 and
  // passed every engine unit test regardless, because `trail()` — the only
  // reader — did not exist either. All three optional columns are NULL.
  CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("links add --json emits the ok-envelope WITHOUT created_at") {
  auto const fx = make_fixture("addjson");
  seed(fx);

  auto const added = dispatch(fx, {"links", "add", "task:1", "plan:2", "--relationship", "derives-from", "--json"});
  CHECK(added.code == 0);
  // `ok` present, `created_at` ABSENT — this is not the `links list`
  // envelope and the two are not interchangeable.
  CHECK(added.out == R"({"ok":true,"id":1,"from_kind":"task","from_id":1,"to_kind":"plan","to_id":2,)"
                     R"("relationship":"derives-from"})"
                     "\n");
  CHECK_FALSE(added.out.contains("created_at"));

  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|task|1|plan|2|derives-from");
}

TEST_CASE("links add accepts every relationship the column allows and refuses the rest") {
  auto const fx = make_fixture("rels");
  seed(fx);

  // All seven, each onto a distinct target so none collides.
  for (auto const& [target, rel] : std::vector<std::pair<std::string, std::string>>{{"task:2", "derives-from"},
                                                                                    {"task:3", "depends-on"},
                                                                                    {"task:4", "addresses"},
                                                                                    {"task:5", "verifies"},
                                                                                    {"task:6", "cites"},
                                                                                    {"task:7", "supersedes"},
                                                                                    {"task:8", "touches"}}) {
    INFO("relationship: " << rel);
    CHECK(dispatch(fx, {"links", "add", "task:1", target, "--relationship", rel}).code == 0);
  }

  {
    auto conn = open_db(fx);
    CHECK(link_rows(conn) == "1|task|1|task|2|derives-from;"
                             "2|task|1|task|3|depends-on;"
                             "3|task|1|task|4|addresses;"
                             "4|task|1|task|5|verifies;"
                             "5|task|1|task|6|cites;"
                             "6|task|1|task|7|supersedes;"
                             "7|task|1|task|8|touches");
    CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>;"
                                   "link|2|<NULL>|<NULL>|<NULL>;"
                                   "link|3|<NULL>|<NULL>|<NULL>;"
                                   "link|4|<NULL>|<NULL>|<NULL>;"
                                   "link|5|<NULL>|<NULL>|<NULL>;"
                                   "link|6|<NULL>|<NULL>|<NULL>;"
                                   "link|7|<NULL>|<NULL>|<NULL>");
  }

  // `blocks` is the PRE-MIGRATION-00033 spelling and is refused, not
  // aliased: `A --blocks--> B` stored "A depends on B", so accepting the
  // word would restore the direction inversion 00033 exists to fix.
  auto const blocks = dispatch(fx, {"links", "add", "plan:1", "plan:2", "--relationship", "blocks"});
  CHECK(blocks.code == 2);
  CHECK(blocks.err == "error: unknown relationship 'blocks'\n");
  CHECK(blocks.out.empty());

  auto const bogus = dispatch(fx, {"links", "add", "plan:1", "plan:2", "--relationship", "relates"});
  CHECK(bogus.code == 2);
  CHECK(bogus.err == "error: unknown relationship 'relates'\n");

  // NEITHER refusal half-wrote. The seven above are still exactly seven.
  auto conn = open_db(fx);
  CHECK(link_rows(conn).ends_with("7|task|1|task|8|touches"));
  CHECK(query_rows(conn, "select count(*) from entity_links", 1) == "7");
  CHECK(query_rows(conn, "select count(*) from audit_log where entity_kind = 'entity_link'", 1) == "7");
}

TEST_CASE("links add refuses a duplicate at exit 1 -- NOT the exit-6 precondition bucket") {
  auto const fx = make_fixture("dup");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0);
  auto const again = dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"});
  // The obvious mapping (`already_exists`) is exit 6 and would be wrong.
  CHECK(again.code == 1);
  CHECK(again.err == "error: link task:1 -> task:2 [cites] already exists\n");
  CHECK(again.out.empty());

  // The refusal wrote NOTHING — not a second edge and not a second audit
  // row. A duplicate that recorded an audit row would be invisible to
  // stdout and would corrupt every later `links trail`.
  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|task|1|task|2|cites");
  CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("links add names WHICH endpoint is missing, and writes nothing") {
  auto const fx = make_fixture("endpoint");
  seed(fx);

  auto const bad_to = dispatch(fx, {"links", "add", "task:1", "task:999", "--relationship", "cites"});
  CHECK(bad_to.code == 1);
  CHECK(bad_to.err == "error: task:999 not found\n");

  auto const bad_from = dispatch(fx, {"links", "add", "plan:999", "task:1", "--relationship", "cites"});
  CHECK(bad_from.code == 1);
  CHECK(bad_from.err == "error: plan:999 not found\n");

  // The discrimination that proves the message names the RIGHT side: with
  // BOTH endpoints missing the oracle reports the FROM one, because `from`
  // is checked first. A handler that reported `to` unconditionally would
  // pass both cases above.
  auto const both = dispatch(fx, {"links", "add", "plan:999", "task:998", "--relationship", "cites"});
  CHECK(both.code == 1);
  CHECK(both.err == "error: plan:999 not found\n");

  auto conn = open_db(fx);
  CHECK(link_rows(conn).empty());
  CHECK(link_audit_rows(conn).empty());
}

TEST_CASE("links add refuses malformed and slug refs with per-POSITIONAL wording") {
  auto const fx = make_fixture("refs");
  seed(fx);

  auto const bad_kind = dispatch(fx, {"links", "add", "nosuch:1", "task:1", "--relationship", "cites"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: invalid from-ref 'nosuch:1': expected kind:integer-id\n");

  auto const no_colon = dispatch(fx, {"links", "add", "task:1", "demo-plan", "--relationship", "cites"});
  CHECK(no_colon.code == 2);
  CHECK(no_colon.err == "error: invalid to-ref 'demo-plan': expected kind:integer-id\n");

  // The two positionals even carry DIFFERENT EXAMPLES in the slug refusal
  // — `task:42` on the from side, `plan:42` on the to side. A shared
  // message would pass one of these and fail the other.
  auto const from_slug = dispatch(fx, {"links", "add", "plan:demo-plan", "task:1", "--relationship", "cites"});
  CHECK(from_slug.code == 2);
  CHECK(from_slug.err == "error: slug refs are not supported; use kind:integer-id (e.g. task:42)\n");

  auto const to_slug = dispatch(fx, {"links", "add", "task:1", "plan:demo-plan", "--relationship", "cites"});
  CHECK(to_slug.code == 2);
  CHECK(to_slug.err == "error: slug refs are not supported; use kind:integer-id (e.g. plan:42)\n");

  auto conn = open_db(fx);
  CHECK(link_rows(conn).empty());
  CHECK(link_audit_rows(conn).empty());
}

// ===========================================================================
// links list
// ===========================================================================

TEST_CASE("links list is an OR over BOTH endpoint columns and its filter EXCLUDES") {
  auto const fx = make_fixture("list");
  seed(fx);

  // Five distinct subjects, deliberately asymmetric: task:1 has outbound
  // AND inbound edges, task:2 only inbound, task:3 only outbound, plan:1
  // a mix across kinds, and task:8 none at all. An inert filter returns
  // the SAME set for all five; a uniformly-applied one returns empty for
  // all five; only a correctly-branching one gives five DIFFERENT answers.
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "depends-on"}).code == 0);    // 1
  REQUIRE(dispatch(fx, {"links", "add", "task:3", "task:1", "--relationship", "derives-from"}).code == 0);  // 2
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "plan:1", "--relationship", "cites"}).code == 0);         // 3
  REQUIRE(dispatch(fx, {"links", "add", "plan:1", "question:1", "--relationship", "addresses"}).code == 0); // 4
  REQUIRE(dispatch(fx, {"links", "add", "task:4", "task:5", "--relationship", "touches"}).code == 0);       // 5 — decoy

  // Subject 1: outbound first (ascending by id), then inbound. The id
  // column's FORCED `+` sign is the oracle's.
  auto const one = dispatch(fx, {"links", "list", "task:1"});
  CHECK(one.code == 0);
  CHECK(one.out == "id     direction  relationship     peer\n"
                   "+1     from       depends-on       task:2\n"
                   "+3     from       cites            plan:1\n"
                   "+2     to         derives-from     task:3\n");

  // Subject 2: INBOUND ONLY. Different set, and `direction` is `to`.
  auto const two = dispatch(fx, {"links", "list", "task:2"});
  CHECK(two.code == 0);
  CHECK(two.out == "id     direction  relationship     peer\n"
                   "+1     to         depends-on       task:1\n");

  // Subject 3: OUTBOUND ONLY.
  auto const three = dispatch(fx, {"links", "list", "task:3"});
  CHECK(three.code == 0);
  CHECK(three.out == "id     direction  relationship     peer\n"
                     "+2     from       derives-from     task:1\n");

  // Subject plan:1: one of each, across kinds — proves the filter matches
  // on the KIND column too and not on the id alone. `task:1` and `plan:1`
  // are different subjects that share id 1.
  auto const plan = dispatch(fx, {"links", "list", "plan:1"});
  CHECK(plan.code == 0);
  CHECK(plan.out == "id     direction  relationship     peer\n"
                    "+4     from       addresses        question:1\n"
                    "+3     to         cites            task:1\n");

  // Subject 8: EMPTY, as a WORD — and the JSON empty case is zero bytes.
  auto const none = dispatch(fx, {"links", "list", "task:8"});
  CHECK(none.code == 0);
  CHECK(none.out == "no links for task:8\n");
  auto const none_json = dispatch(fx, {"links", "list", "task:8", "--json"});
  CHECK(none_json.code == 0);
  CHECK(none_json.out.empty()); // ZERO BYTES — not `[]`, not `\n`.
  CHECK(none_json.err.empty());

  // THE SURVIVOR. Edge 5 (`task:4 -> task:5`) appeared in NONE of the five
  // listings above, and it is still in the table. That is what makes the
  // exclusions real rather than an artifact of the rows never existing.
  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|task|1|task|2|depends-on;"
                           "2|task|3|task|1|derives-from;"
                           "3|task|1|plan|1|cites;"
                           "4|plan|1|question|1|addresses;"
                           "5|task|4|task|5|touches");
}

TEST_CASE("links list lists a SELF-LINK exactly once, as from") {
  auto const fx = make_fixture("self");
  seed(fx);

  // A self-link satisfies BOTH half-queries. Without de-duplication the
  // listing reports two edges where the table holds one.
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:1", "--relationship", "cites"}).code == 0);

  auto const listed = dispatch(fx, {"links", "list", "task:1"});
  CHECK(listed.code == 0);
  CHECK(listed.out == "id     direction  relationship     peer\n"
                      "+1     from       cites            task:1\n");

  auto const json = dispatch(fx, {"links", "list", "task:1", "--json"});
  CHECK(json.code == 0);
  CHECK(std::ranges::count(json.out, '\n') == 1);

  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|task|1|task|1|cites");
}

TEST_CASE("links list --json is NDJSON carrying created_at and no ok field") {
  auto const fx = make_fixture("listjson");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "depends-on"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "plan:1", "--relationship", "cites"}).code == 0);

  auto const json = dispatch(fx, {"links", "list", "task:1", "--json"});
  CHECK(json.code == 0);
  // One object per LINE, not a JSON array.
  CHECK_FALSE(json.out.starts_with("["));
  CHECK(std::ranges::count(json.out, '\n') == 2);
  CHECK(json.out.contains(R"({"id":1,"from_kind":"task","from_id":1,"to_kind":"task","to_id":2,)"
                          R"("relationship":"depends-on","created_at":")"));
  CHECK(json.out.contains(R"({"id":2,"from_kind":"task","from_id":1,"to_kind":"plan","to_id":1,)"
                          R"("relationship":"cites","created_at":")"));
  CHECK_FALSE(json.out.contains("\"ok\""));
}

TEST_CASE("links list does NOT check existence, but does refuse a malformed ref") {
  auto const fx = make_fixture("listrefs");
  seed(fx);

  // A missing entity LISTS EMPTY at exit 0 — only the write verbs check.
  auto const missing = dispatch(fx, {"links", "list", "task:999"});
  CHECK(missing.code == 0);
  CHECK(missing.out == "no links for task:999\n");
  CHECK(missing.err.empty());

  // `links list`'s two refusals are worded DIFFERENTLY from `links add`'s
  // and from `<entity> link`'s. All three were captured separately.
  auto const bad = dispatch(fx, {"links", "list", "nosuch:1"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: invalid ref 'nosuch:1': expected kind:id (id must be an integer)\n");

  auto const slug = dispatch(fx, {"links", "list", "plan:demo-plan"});
  CHECK(slug.code == 2);
  CHECK(slug.err == "error: links list requires a numeric id (got slug 'plan:demo-plan')\n");

  auto conn = open_db(fx);
  CHECK(link_rows(conn).empty());
}

// ===========================================================================
// links remove
// ===========================================================================

TEST_CASE("links remove deletes the edge, records unlink, and leaves the audit rows behind") {
  auto const fx = make_fixture("remove");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:3", "--relationship", "cites"}).code == 0);

  auto const removed = dispatch(fx, {"links", "remove", "1"});
  CHECK(removed.code == 0);
  CHECK(removed.out == "entity link 1 removed\n");
  CHECK(removed.err.empty());

  {
    auto conn = open_db(fx);
    // Edge 1 gone, edge 2 SURVIVES — the delete's `where id = ?` is proven
    // to discriminate rather than to truncate.
    CHECK(link_rows(conn) == "2|task|1|task|3|cites");
    // Both `link` rows remain AND an `unlink` row joins them. The audit
    // trail deliberately OUTLIVES the row it describes.
    CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>;"
                                   "link|2|<NULL>|<NULL>|<NULL>;"
                                   "unlink|1|<NULL>|<NULL>|<NULL>");
  }

  // ...which makes the removed link's trail UNREACHABLE even though its
  // two audit rows are still on disk. Reproduced, not fixed.
  auto const trail = dispatch(fx, {"links", "trail", "1"});
  CHECK(trail.code == 1);
  CHECK(trail.err == "error: entity link 1 not found\n");
}

TEST_CASE("links remove refuses a missing id and writes nothing") {
  auto const fx = make_fixture("removemiss");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0);

  auto const missing = dispatch(fx, {"links", "remove", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: entity link 999 not found\n");
  CHECK(missing.out.empty());

  auto const bad = dispatch(fx, {"links", "remove", "notanint"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: link id must be an integer, got 'notanint'\n");

  // Neither refusal touched either table — in particular neither wrote a
  // spurious `unlink` audit row for a link that was never removed.
  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|task|1|task|2|cites");
  CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("links remove --json emits the ok-envelope") {
  auto const fx = make_fixture("removejson");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0);
  auto const removed = dispatch(fx, {"links", "remove", "1", "--json"});
  CHECK(removed.code == 0);
  CHECK(removed.out == "{\"ok\":true,\"id\":1}\n");

  auto conn = open_db(fx);
  CHECK(link_rows(conn).empty());
}

// ===========================================================================
// links trail
// ===========================================================================

TEST_CASE("links trail filters on BOTH entity_kind and entity_id, and both decoys survive") {
  auto const fx = make_fixture("trail");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0); // link 1
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:3", "--relationship", "cites"}).code == 0); // link 2
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:4", "--relationship", "cites"}).code == 0); // link 3

  // THE DECOY that proves the `entity_kind` half of the predicate is live:
  // an audit row with entity_id = 1 under a DIFFERENT kind. Dropping
  // `entity_kind` from the WHERE clause pulls this into `links trail 1`.
  // It is seeded by the CLI (`plan create` above already wrote
  // `create|plan|1`), so no raw-SQL fixture is needed — assert it exists.
  {
    auto conn = open_db(fx);
    CHECK(query_rows(conn, "select verb, entity_id from audit_log where entity_kind = 'plan' order by id", 2) ==
          "create|1;create|2");
  }

  // Link 1's trail: EXACTLY its own row. Not link 2's or 3's (same kind,
  // different id) and not `plan|1`'s (same id, different kind).
  auto const one = dispatch(fx, {"links", "trail", "1"});
  CHECK(one.code == 0);
  CHECK(one.err.empty());
  CHECK(one.out.starts_with("id     verb         actor        recorded_at\n"));
  CHECK(std::ranges::count(one.out, '\n') == 2);
  // `(none)` is the LITERAL for a NULL actor, and the id column carries
  // the same forced `+` sign as `links list`.
  CHECK(one.out.contains("link         (none)       "));

  // A DIFFERENT link gives a DIFFERENT single row — an inert entity_id
  // predicate would return all three here and in the case above.
  auto const three = dispatch(fx, {"links", "trail", "3"});
  CHECK(three.code == 0);
  CHECK(std::ranges::count(three.out, '\n') == 2);
  CHECK(one.out != three.out);

  // THE SURVIVORS: every decoy row is still in `audit_log` afterwards, so
  // the exclusions above are real and not an artifact of absent rows.
  auto       conn = open_db(fx);
  auto const all  = all_audit_kinds(conn);
  CHECK(all.contains("plan|1|create"));
  CHECK(all.contains("plan|2|create"));
  CHECK(all.contains("entity_link|1|link"));
  CHECK(all.contains("entity_link|2|link"));
  CHECK(all.contains("entity_link|3|link"));
}

TEST_CASE("links trail distinguishes a MISSING link from an EMPTY trail") {
  auto const fx = make_fixture("trailempty");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0);

  // An existing link whose audit rows were deleted out from under it: the
  // trail is EMPTY and that is a SUCCESS. The two answers have different
  // exit codes and only this pair proves the handler tells them apart.
  {
    auto conn = open_db(fx);
    auto stmt = conn.prepare("delete from audit_log where entity_kind = 'entity_link'");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
  }

  auto const empty = dispatch(fx, {"links", "trail", "1"});
  CHECK(empty.code == 0);
  CHECK(empty.out == "no audit trail for entity_link:1\n");
  CHECK(empty.err.empty());

  auto const empty_json = dispatch(fx, {"links", "trail", "1", "--json"});
  CHECK(empty_json.code == 0);
  CHECK(empty_json.out.empty()); // ZERO BYTES, like `links list --json`.

  // ...versus a link that does not exist at all.
  auto const missing = dispatch(fx, {"links", "trail", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: entity link 999 not found\n");
  CHECK(missing.out.empty());

  auto const bad = dispatch(fx, {"links", "trail", "notanint"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: link id must be an integer, got 'notanint'\n");
}

TEST_CASE("links trail --json renders the three nullable columns as null, not empty string") {
  auto const fx = make_fixture("trailjson");
  seed(fx);

  REQUIRE(dispatch(fx, {"links", "add", "task:1", "task:2", "--relationship", "cites"}).code == 0);

  auto const json = dispatch(fx, {"links", "trail", "1", "--json"});
  CHECK(json.code == 0);
  CHECK(std::ranges::count(json.out, '\n') == 1);
  CHECK(json.out.starts_with(R"({"id":)"));
  // The distinction that matters: `null`, NOT `""`. A read side that
  // folded SQL NULL onto the empty string produces `"actor":""` here and
  // `(none)` -> a blank column in the text form, and every verb/entity_id
  // assertion still passes.
  CHECK(json.out.contains(R"("verb":"link","entity_kind":"entity_link","entity_id":1,)"
                          R"("actor":null,"scope":null,"summary":null,"recorded_at":")"));
  CHECK_FALSE(json.out.contains(R"("actor":"")"));
}

// ===========================================================================
// plan link / task link / question link — the shared arm
// ===========================================================================

TEST_CASE("the three <entity> link leaves share one shape and differ only where the oracle does") {
  auto const fx = make_fixture("entity");
  seed(fx);

  auto const planned = dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "depends-on"});
  CHECK(planned.code == 0);
  // DOUBLE spaces around the `[relationship]` group, and an entirely
  // different sentence from `links add`'s for the same insert.
  CHECK(planned.out == "linked plan:1 -> task:1  [depends-on]  (link id: 1)\n");

  auto const tasked = dispatch(fx, {"task", "link", "2", "plan:2", "--relationship", "derives-from"});
  CHECK(tasked.code == 0);
  CHECK(tasked.out == "linked task:2 -> plan:2  [derives-from]  (link id: 2)\n");

  auto const asked = dispatch(fx, {"question", "link", "1", "task:3", "--relationship", "addresses"});
  CHECK(asked.code == 0);
  CHECK(asked.out == "linked question:1 -> task:3  [addresses]  (link id: 3)\n");

  auto conn = open_db(fx);
  // The from_kind is the VERB's, never the ref's — the whole point of
  // three leaves over one table.
  CHECK(link_rows(conn) == "1|plan|1|task|1|depends-on;"
                           "2|task|2|plan|2|derives-from;"
                           "3|question|1|task|3|addresses");
  CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>;"
                                 "link|2|<NULL>|<NULL>|<NULL>;"
                                 "link|3|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("each <entity> link leaf emits its OWN subject JSON key") {
  auto const fx = make_fixture("entityjson");
  seed(fx);

  auto const planned = dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "cites", "--json"});
  CHECK(planned.code == 0);
  CHECK(planned.out == R"({"ok":true,"id":1,"plan_id":1,"to_kind":"task","to_id":1,"relationship":"cites"})"
                       "\n");

  auto const tasked = dispatch(fx, {"task", "link", "2", "plan:2", "--relationship", "cites", "--json"});
  CHECK(tasked.code == 0);
  CHECK(tasked.out == R"({"ok":true,"id":2,"task_id":2,"to_kind":"plan","to_id":2,"relationship":"cites"})"
                      "\n");

  auto const asked = dispatch(fx, {"question", "link", "1", "task:3", "--relationship", "cites", "--json"});
  CHECK(asked.code == 0);
  CHECK(asked.out == R"({"ok":true,"id":3,"question_id":1,"to_kind":"task","to_id":3,"relationship":"cites"})"
                     "\n");

  // The from-side columns are ABSENT from this envelope entirely — it is
  // not `links add`'s with a renamed field.
  CHECK_FALSE(planned.out.contains("from_kind"));
  CHECK_FALSE(planned.out.contains("created_at"));

  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "1|plan|1|task|1|cites;"
                           "2|task|2|plan|2|cites;"
                           "3|question|1|task|3|cites");
}

TEST_CASE("plan link spells its duplicate arrow with U+2192 and its two siblings do not") {
  auto const fx = make_fixture("arrow");
  seed(fx);

  REQUIRE(dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "depends-on"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "link", "2", "plan:2", "--relationship", "derives-from"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "link", "1", "task:3", "--relationship", "addresses"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:4", "task:5", "--relationship", "cites"}).code == 0);

  // An oracle INCONSISTENCY, reproduced rather than harmonised: only
  // `plan link` uses the unicode arrow. `handlers/plan/link.zig` writes
  // `\u{2192}`; its two siblings and `links add` write `->`.
  auto const planned = dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "depends-on"});
  CHECK(planned.code == 1);
  CHECK(planned.err == "error: link plan:1 → task:1 [depends-on] already exists\n");

  auto const tasked = dispatch(fx, {"task", "link", "2", "plan:2", "--relationship", "derives-from"});
  CHECK(tasked.code == 1);
  CHECK(tasked.err == "error: link task:2 -> plan:2 [derives-from] already exists\n");

  auto const asked = dispatch(fx, {"question", "link", "1", "task:3", "--relationship", "addresses"});
  CHECK(asked.code == 1);
  CHECK(asked.err == "error: link question:1 -> task:3 [addresses] already exists\n");

  auto const added = dispatch(fx, {"links", "add", "task:4", "task:5", "--relationship", "cites"});
  CHECK(added.code == 1);
  CHECK(added.err == "error: link task:4 -> task:5 [cites] already exists\n");

  // The three ASCII ones are byte-identical in their arrow; the plan one
  // is not. Stated as an explicit discrimination so a future "tidy-up"
  // that harmonises them fails here rather than silently diverging.
  CHECK_FALSE(planned.err.contains(" -> "));
  CHECK(tasked.err.contains(" -> "));
  CHECK(asked.err.contains(" -> "));
  CHECK(added.err.contains(" -> "));

  // Four refusals, and the table still holds exactly the four originals.
  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select count(*) from entity_links", 1) == "4");
  CHECK(query_rows(conn, "select count(*) from audit_log where entity_kind = 'entity_link'", 1) == "4");
}

TEST_CASE("<entity> link parses its subject id BEFORE it requires --relationship") {
  auto const fx = make_fixture("order");
  seed(fx);

  // Both refusals are reachable from one argv. The ORDER decides which
  // fires, and it is oracle-captured: a non-integer subject id wins even
  // though `--relationship` is also absent.
  auto const bad_id = dispatch(fx, {"plan", "link", "notanint", "task:1"});
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: plan id must be an integer, got 'notanint'\n");

  auto const bad_task = dispatch(fx, {"task", "link", "notanint", "plan:1"});
  CHECK(bad_task.err == "error: task id must be an integer, got 'notanint'\n");
  auto const bad_q = dispatch(fx, {"question", "link", "notanint", "task:1"});
  CHECK(bad_q.err == "error: question id must be an integer, got 'notanint'\n");

  // With a VALID id the next refusal surfaces — proving the first one was
  // ordering and not a blanket rejection.
  auto const no_rel = dispatch(fx, {"plan", "link", "1", "task:1"});
  CHECK(no_rel.code == 2);
  CHECK(no_rel.err == "error: --relationship is required\n");

  // ...then the relationship VALUE, then the ref, then existence.
  auto const bad_rel = dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "nope"});
  CHECK(bad_rel.code == 2);
  CHECK(bad_rel.err == "error: unknown relationship 'nope'\n");

  auto const bad_ref = dispatch(fx, {"plan", "link", "1", "nosuch:1", "--relationship", "cites"});
  CHECK(bad_ref.code == 2);
  CHECK(bad_ref.err == "error: invalid ref 'nosuch:1': expected kind:integer-id\n");

  auto const slug_ref = dispatch(fx, {"plan", "link", "1", "plan:demo-plan", "--relationship", "cites"});
  CHECK(slug_ref.code == 2);
  CHECK(slug_ref.err == "error: slug refs are not supported; use kind:integer-id (e.g. plan:42)\n");

  auto const missing_plan = dispatch(fx, {"plan", "link", "999", "task:1", "--relationship", "cites"});
  CHECK(missing_plan.code == 1);
  CHECK(missing_plan.err == "error: plan:999 not found\n");

  auto const missing_to = dispatch(fx, {"plan", "link", "1", "task:999", "--relationship", "cites"});
  CHECK(missing_to.code == 1);
  CHECK(missing_to.err == "error: task:999 not found\n");

  // NINE refusals, zero writes.
  auto conn = open_db(fx);
  CHECK(link_rows(conn).empty());
  CHECK(link_audit_rows(conn).empty());
}

TEST_CASE("<entity> link accepts --scope and is completely inert on it -- by design") {
  auto const fx = make_fixture("scope");
  seed(fx);

  // Link verbs are UNGUARDED BY DESIGN (CLAUDE.md § cross-scope-guard):
  // they create edges that may legitimately cross scopes. The oracle
  // accepts ANY `--scope` value, including one that resolves to nothing,
  // and writes the link regardless. Verified by running both.
  //
  // This is asserted rather than left implicit because it is exactly the
  // shape that IS a defect elsewhere (an accepted-but-ignored filter), and
  // the difference is that nothing here is being filtered: a reader must
  // be able to tell the two apart from the test.
  auto const global_scope = dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "cites", "--scope", "global"});
  CHECK(global_scope.code == 0);
  CHECK(global_scope.out == "linked plan:1 -> task:1  [cites]  (link id: 1)\n");

  auto const nonsense = dispatch(fx, {"plan", "link", "1", "task:2", "--relationship", "cites", "--scope", "nosuchscope"});
  CHECK(nonsense.code == 0);
  CHECK(nonsense.err.empty());
  CHECK(nonsense.out == "linked plan:1 -> task:2  [cites]  (link id: 2)\n");

  auto const absent = dispatch(fx, {"plan", "link", "1", "task:3", "--relationship", "cites"});
  CHECK(absent.code == 0);

  auto conn = open_db(fx);
  // All THREE rows landed, and none carries a scope — `entity_links` has
  // no scope column at all, which is the structural reason the flag can
  // only ever be inert here.
  CHECK(link_rows(conn) == "1|plan|1|task|1|cites;"
                           "2|plan|1|task|2|cites;"
                           "3|plan|1|task|3|cites");
}

TEST_CASE("an edge written by <entity> link is readable through links list and links trail") {
  auto const fx = make_fixture("roundtrip");
  seed(fx);

  // The whole point of landing the seven together: they are ONE surface
  // over ONE table, and a port that split the write and read halves would
  // pass every single-verb case above.
  REQUIRE(dispatch(fx, {"plan", "link", "1", "task:1", "--relationship", "depends-on"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "link", "1", "plan:1", "--relationship", "addresses"}).code == 0);

  auto const listed = dispatch(fx, {"links", "list", "plan:1"});
  CHECK(listed.code == 0);
  CHECK(listed.out == "id     direction  relationship     peer\n"
                      "+1     from       depends-on       task:1\n"
                      "+2     to         addresses        question:1\n");

  auto const trail = dispatch(fx, {"links", "trail", "1"});
  CHECK(trail.code == 0);
  CHECK(std::ranges::count(trail.out, '\n') == 2);

  // ...and `links remove` closes the loop on an edge `plan link` opened.
  auto const removed = dispatch(fx, {"links", "remove", "1"});
  CHECK(removed.code == 0);

  auto conn = open_db(fx);
  CHECK(link_rows(conn) == "2|question|1|plan|1|addresses");
  CHECK(link_audit_rows(conn) == "link|1|<NULL>|<NULL>|<NULL>;"
                                 "link|2|<NULL>|<NULL>|<NULL>;"
                                 "unlink|1|<NULL>|<NULL>|<NULL>");
}
