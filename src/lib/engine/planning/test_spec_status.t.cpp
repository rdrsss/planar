// @file test_spec_status.t.cpp
// @brief Unit tests for `planar.engine.planning.test_spec_status` (plan 996,
// task 6095).
//
// ORACLE PROVENANCE. Every count and every byte below was captured by RUNNING
// the Zig binary against a seeded scratch database, never from `--help` and
// never from reading the Zig source. All rows were written with FIXED
// timestamps so the captures are byte-stable and re-runnable:
//
//   mkdir -p /tmp/fx/repo && cd /tmp/fx/repo && git init -q .
//   export PLANAR_DB=/tmp/fx/p.db PLANAR_CONFIG_PATH=/tmp/fx/p.toml
//   ./zig/zig-out/bin/planar init
//   sqlite3 /tmp/fx/p.db "
//     -- anchor plan 1 'P'/'p'; two milestones deriving from it, the second
//     -- with a title past the 32-char truncation boundary.
//     insert into plans (id,scope_kind,title,slug,status,parent_plan_id,...) values
//       (1,'global','P','p','draft',null,...),
//       (2,'global','M1 Foundation','m1','draft',1,...),
//       (3,'global','M2 A Very Long Milestone Title That Exceeds Limit','m2','draft',1,...);
//     insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values
//       ('plan',2,'plan',1,'derives-from'), ('plan',3,'plan',1,'derives-from');
//     insert into tasks (id,scope_kind,plan_id,title,slug,status,...) values
//       (1,'global',1,'anchor task','at','todo',...),
//       (2,'global',2,'m1 task a','m1a','todo',...),
//       (3,'global',2,'m1 task b',NULL,'todo',...),     <-- no slug
//       (4,'global',3,'m2 task a','m2a','todo',...);
//     insert into test_scenarios (id,scope_kind,title,status,...) values
//       (1,'global','Happy path — basic',...),   (2,'global','Error: bad input',...),
//       (3,'global','Edge case wrap',...),       (4,'global','Empty / null set',...),
//       (5,'global','Something else',...),       (6,'global','NULL input',...),
//       (7,'global','detached scenario',...);    <-- attached to nothing
//     -- scenarios 1-6 derive-from the ANCHOR; 7 does not.
//     -- verifies edges: 1->{2,4}, 2->2, 3->4, 4->2, 5->2, 6->2
//   "
//
//   $Z test-spec status 1 --json
//     -> {"plan_id":1,"title":"P","total_tasks":1,"tasks_with_slug":1,"tasks_covered":0,
//         "happy":0,"empty":0,"error":0,"edge":0,"other":0}
//        {"plan_id":2,"title":"M1 Foundation","total_tasks":2,"tasks_with_slug":1,
//         "tasks_covered":1,"happy":1,"empty":2,"error":1,"edge":0,"other":1}
//        {"plan_id":3,"title":"M2 A Very Long Milestone Title That Exceeds Limit",
//         "total_tasks":1,"tasks_with_slug":1,"tasks_covered":1,
//         "happy":1,"empty":0,"error":0,"edge":1,"other":0}
//        {"anchor_plan_id":1,"total_tasks":4,"tasks_with_slug":3,"tasks_covered":2,
//         "total_scenarios":6}
//
//   $Z test-spec status 1
//     -> 'test-spec status for plan 1 (p)'
//        '  milestone                        tasks  slug   cov   happy empty error  edge other'
//        '  P                                   +1    +1    +0      +0    +0    +0    +0    +0'
//        '  M1 Foundation                       +2    +1    +1      +1    +2    +1    +0    +1'
//        '  M2 A Very Long Milestone Titl...    +1    +1    +1      +1    +0    +0    +1    +0'
//        ''
//        '  6 scenarios total; 2 of 3 slug-bearing tasks covered (4 total tasks).'
//
//   $Z test-spec status p --json    -> identical to `status 1 --json`
//   $Z test-spec status m1 --json   -> exit 1, error: plan 'm1' not found
//   $Z test-spec status 2 --json    -> exit 1, error: plan '2' not found
//   $Z test-spec status 99 --json   -> exit 1, error: plan '99' not found
//   $Z test-spec status nosuch --json -> exit 1, error: plan 'nosuch' not found
//   [empty database] $Z test-spec status 1 --json -> exit 1, error: plan '1' not found
//
// HAZARD 4 / the width probe. A fourth milestone with 12345 tasks was added
// to establish the count-column rule empirically:
//
//   '  M3                               +12345 +12345    +0      +0    +0    +0    +0    +0'
//
// which pins BOTH surprises: the explicit `+` sign on every count, and that a
// too-wide count overflows its five-wide field (pushing the rest of the row
// right) instead of being truncated.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.test_spec_status;

namespace {

namespace tss = planar::engine::planning::test_spec_status;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_tss_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

/// @brief Reproduce the oracle fixture transcribed in this file's header,
/// row for row.
auto seed_fixture(planar::db::connection& conn) -> void {
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(1,'global','P','p','draft',null),"
             "(2,'global','M1 Foundation','m1','draft',1),"
             "(3,'global','M2 A Very Long Milestone Title That Exceeds Limit','m2','draft',1)");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('plan',2,'plan',1,'derives-from'),('plan',3,'plan',1,'derives-from')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status) values "
             "(1,'global',1,'anchor task','at','todo'),"
             "(2,'global',2,'m1 task a','m1a','todo'),"
             "(3,'global',2,'m1 task b',null,'todo'),"
             "(4,'global',3,'m2 task a','m2a','todo')");
  exec(conn, "insert into test_scenarios (id,scope_kind,title,status) values "
             "(1,'global','Happy path \xe2\x80\x94 basic','draft'),"
             "(2,'global','Error: bad input','draft'),"
             "(3,'global','Edge case wrap','draft'),"
             "(4,'global','Empty / null set','draft'),"
             "(5,'global','Something else','draft'),"
             "(6,'global','NULL input','draft'),"
             "(7,'global','detached scenario','draft')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('test_scenario',1,'plan',1,'derives-from'),"
             "('test_scenario',2,'plan',1,'derives-from'),"
             "('test_scenario',3,'plan',1,'derives-from'),"
             "('test_scenario',4,'plan',1,'derives-from'),"
             "('test_scenario',5,'plan',1,'derives-from'),"
             "('test_scenario',6,'plan',1,'derives-from')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('test_scenario',1,'task',2,'verifies'),"
             "('test_scenario',1,'task',4,'verifies'),"
             "('test_scenario',2,'task',2,'verifies'),"
             "('test_scenario',3,'task',4,'verifies'),"
             "('test_scenario',4,'task',2,'verifies'),"
             "('test_scenario',5,'task',2,'verifies'),"
             "('test_scenario',6,'task',2,'verifies')");
}

// The oracle's `test-spec status 1 --json`, byte for byte.
constexpr std::string_view k_fixture_json =
    "{\"plan_id\":1,\"title\":\"P\",\"total_tasks\":1,\"tasks_with_slug\":1,\"tasks_covered\":0,"
    "\"happy\":0,\"empty\":0,\"error\":0,\"edge\":0,\"other\":0}\n"
    "{\"plan_id\":2,\"title\":\"M1 Foundation\",\"total_tasks\":2,\"tasks_with_slug\":1,\"tasks_covered\":1,"
    "\"happy\":1,\"empty\":2,\"error\":1,\"edge\":0,\"other\":1}\n"
    "{\"plan_id\":3,\"title\":\"M2 A Very Long Milestone Title That Exceeds Limit\",\"total_tasks\":1,"
    "\"tasks_with_slug\":1,\"tasks_covered\":1,\"happy\":1,\"empty\":0,\"error\":0,\"edge\":1,\"other\":0}\n"
    "{\"anchor_plan_id\":1,\"total_tasks\":4,\"tasks_with_slug\":3,\"tasks_covered\":2,\"total_scenarios\":6}\n";

// The oracle's `test-spec status 1`, byte for byte.
constexpr std::string_view k_fixture_text =
    "test-spec status for plan 1 (p)\n"
    "  milestone                        tasks  slug   cov   happy empty error  edge other\n"
    "  P                                   +1    +1    +0      +0    +0    +0    +0    +0\n"
    "  M1 Foundation                       +2    +1    +1      +1    +2    +1    +0    +1\n"
    "  M2 A Very Long Milestone Titl...    +1    +1    +1      +1    +0    +0    +1    +0\n"
    "\n"
    "  6 scenarios total; 2 of 3 slug-bearing tasks covered (4 total tasks).\n";

} // namespace

TEST_CASE("test_spec_status: classify_bucket matches the oracle's five buckets", "[planning]") {
  REQUIRE(tss::classify_bucket("Happy path \xe2\x80\x94 basic") == tss::bucket::happy);
  REQUIRE(tss::classify_bucket("Error: bad input") == tss::bucket::error_case);
  REQUIRE(tss::classify_bucket("Edge case wrap") == tss::bucket::edge);
  REQUIRE(tss::classify_bucket("Something else") == tss::bucket::other);

  // Three distinct prefixes all reach `empty` -- this is why the fixture's
  // `empty` column reads 2 for a milestone with only one `Empty ...` scenario
  // and one `NULL ...` scenario.
  REQUIRE(tss::classify_bucket("Empty / null set") == tss::bucket::empty);
  REQUIRE(tss::classify_bucket("Empty set") == tss::bucket::empty);
  REQUIRE(tss::classify_bucket("NULL input") == tss::bucket::empty);

  // ASCII case-insensitive.
  REQUIRE(tss::classify_bucket("HAPPY PATH") == tss::bucket::happy);
  REQUIRE(tss::classify_bucket("eRrOr") == tss::bucket::error_case);

  // Leading spaces and tabs are trimmed; a leading NEWLINE is not (the Zig
  // original trims exactly " \t"), so it falls to `other`.
  REQUIRE(tss::classify_bucket("   \tHappy path") == tss::bucket::happy);
  REQUIRE(tss::classify_bucket("\nHappy path") == tss::bucket::other);

  // Prefix, not substring: a bucket word mid-title does not classify.
  REQUIRE(tss::classify_bucket("regression: happy path") == tss::bucket::other);
  REQUIRE(tss::classify_bucket("") == tss::bucket::other);
}

TEST_CASE("test_spec_status: compute reproduces the oracle's aggregates exactly", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture(conn);

  auto report = tss::compute(conn, 1);
  REQUIRE(report.has_value());
  REQUIRE(report->milestones.size() == 3);

  // Anchor: one task, slug-bearing, no scenario verifies it.
  REQUIRE(report->milestones[0].plan_id == 1);
  REQUIRE(report->milestones[0].title == "P");
  REQUIRE(report->milestones[0].total_tasks == 1);
  REQUIRE(report->milestones[0].tasks_with_slug == 1);
  REQUIRE(report->milestones[0].tasks_covered == 0);

  // M1: two tasks, one NULL-slug, and the single covered task (task 2) is
  // verified by FIVE scenarios -- so tasks_covered is 1 while the bucket
  // columns sum to 5. This is the discriminating row: a per-task tally would
  // read happy:1,empty:1,error:1,other:1 instead.
  REQUIRE(report->milestones[1].plan_id == 2);
  REQUIRE(report->milestones[1].total_tasks == 2);
  REQUIRE(report->milestones[1].tasks_with_slug == 1);
  REQUIRE(report->milestones[1].tasks_covered == 1);
  REQUIRE(report->milestones[1].happy == 1);
  REQUIRE(report->milestones[1].empty == 2);
  REQUIRE(report->milestones[1].error_count == 1);
  REQUIRE(report->milestones[1].edge == 0);
  REQUIRE(report->milestones[1].other == 1);

  REQUIRE(report->milestones[2].plan_id == 3);
  REQUIRE(report->milestones[2].tasks_covered == 1);
  REQUIRE(report->milestones[2].happy == 1);
  REQUIRE(report->milestones[2].edge == 1);

  // The roll-up. total_scenarios counts ATTACHED scenarios (6), not the seven
  // rows in the table and not the seven verifies edges.
  REQUIRE(report->summary.anchor_plan_id == 1);
  REQUIRE(report->summary.total_tasks == 4);
  REQUIRE(report->summary.tasks_with_slug == 3);
  REQUIRE(report->summary.tasks_covered == 2);
  REQUIRE(report->summary.total_scenarios == 6);
}

TEST_CASE("test_spec_status: milestones come from derives-from edges, not parent_plan_id", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture(conn);
  // A child plan by parent_plan_id with NO derives-from edge must not appear
  // as a milestone, and its tasks must not reach the roll-up.
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(4,'global','Orphan Child','oc','draft',1)");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status) values "
             "(5,'global',4,'orphan task','ot','todo')");

  auto report = tss::compute(conn, 1);
  REQUIRE(report.has_value());
  REQUIRE(report->milestones.size() == 3);
  REQUIRE(report->summary.total_tasks == 4);

  // ...and adding the edge (without touching parent_plan_id) DOES bring it in.
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('plan',4,'plan',1,'derives-from')");
  auto after = tss::compute(conn, 1);
  REQUIRE(after.has_value());
  REQUIRE(after->milestones.size() == 4);
  REQUIRE(after->summary.total_tasks == 5);
}

TEST_CASE("test_spec_status: an empty-string slug does not count as slug-bearing", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(1,'global','P','p','draft',null)");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status) values "
             "(1,'global',1,'null slug',null,'todo'),"
             "(2,'global',1,'empty slug','','todo'),"
             "(3,'global',1,'real slug','rs','todo')");

  auto report = tss::compute(conn, 1);
  REQUIRE(report.has_value());
  REQUIRE(report->milestones.size() == 1);
  REQUIRE(report->milestones[0].total_tasks == 3);
  // Only the real slug counts: NULL and '' both fall out.
  REQUIRE(report->milestones[0].tasks_with_slug == 1);
}

TEST_CASE("test_spec_status: an attached scenario verifying nothing still counts", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(1,'global','P','p','draft',null)");
  exec(conn, "insert into test_scenarios (id,scope_kind,title,status) values "
             "(1,'global','Happy path lonely','draft'),"
             "(2,'global','Detached','draft')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('test_scenario',1,'plan',1,'derives-from')");

  auto report = tss::compute(conn, 1);
  REQUIRE(report.has_value());
  // total_scenarios counts ATTACHMENT, not coverage: 1, not 0 and not 2.
  REQUIRE(report->summary.total_scenarios == 1);
  REQUIRE(report->summary.tasks_covered == 0);
}

TEST_CASE("test_spec_status: compute on an unknown plan id yields no rows, not an error", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // fetch_anchor is what refuses an unknown plan; compute itself degrades to
  // an empty report. Pinned so the two responsibilities stay separated.
  auto report = tss::compute(conn, 999);
  REQUIRE(report.has_value());
  REQUIRE(report->milestones.empty());
  REQUIRE(report->summary.anchor_plan_id == 999);
  REQUIRE(report->summary.total_tasks == 0);
  REQUIRE(report->summary.total_scenarios == 0);
}

TEST_CASE("test_spec_status: fetch_anchor takes an id or a slug but refuses a milestone", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture(conn);

  auto by_id = tss::fetch_anchor(conn, "1");
  REQUIRE(by_id.has_value());
  REQUIRE(by_id->id == 1);
  REQUIRE(by_id->slug == "p");

  auto by_slug = tss::fetch_anchor(conn, "p");
  REQUIRE(by_slug.has_value());
  REQUIRE(by_slug->id == 1);

  // The surprising half: plan 2 and slug `m1` are a REAL plan, but they are
  // not an anchor (parent_plan_id is not null) and the leaf reports them as
  // "not found" rather than "not an anchor". Oracle-confirmed at exit 1.
  auto milestone_by_id = tss::fetch_anchor(conn, "2");
  REQUIRE_FALSE(milestone_by_id.has_value());
  REQUIRE(milestone_by_id.error() == tss::test_spec_error::not_found);

  auto milestone_by_slug = tss::fetch_anchor(conn, "m1");
  REQUIRE_FALSE(milestone_by_slug.has_value());
  REQUIRE(milestone_by_slug.error() == tss::test_spec_error::not_found);

  auto nothing = tss::fetch_anchor(conn, "99");
  REQUIRE_FALSE(nothing.has_value());
  REQUIRE(nothing.error() == tss::test_spec_error::not_found);

  auto no_slug = tss::fetch_anchor(conn, "nosuch");
  REQUIRE_FALSE(no_slug.has_value());
  REQUIRE(no_slug.error() == tss::test_spec_error::not_found);
}

TEST_CASE("test_spec_status: a numeric argument never falls through to the slug branch", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  // An anchor whose SLUG is the digit string "7" while its id is 1. Looking
  // up "7" must miss (no plan with id 7), NOT find this plan by slug.
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(1,'global','Seven','7','draft',null)");

  auto numeric = tss::fetch_anchor(conn, "7");
  REQUIRE_FALSE(numeric.has_value());
  REQUIRE(numeric.error() == tss::test_spec_error::not_found);

  // ...while a non-numeric slug lookup on the same table works.
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(2,'global','Named','named','draft',null)");
  auto named = tss::fetch_anchor(conn, "named");
  REQUIRE(named.has_value());
  REQUIRE(named->id == 2);
}

TEST_CASE("test_spec_status: --json output matches the oracle byte for byte", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture(conn);

  auto report = tss::compute(conn, 1);
  REQUIRE(report.has_value());
  const auto out = tss::render_json(*report);
  REQUIRE(out == k_fixture_json);
  // NDJSON, not a single document: four newline-terminated lines.
  REQUIRE(std::ranges::count(out, '\n') == 4);
  REQUIRE(out.ends_with("}\n"));
}

TEST_CASE("test_spec_status: text output matches the oracle byte for byte", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture(conn);

  auto report = tss::compute(conn, 1);
  REQUIRE(report.has_value());
  REQUIRE(tss::render_text("p", *report) == k_fixture_text);
}

TEST_CASE("test_spec_status: counts carry an explicit + sign and overflow their column", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into plans (id,scope_kind,title,slug,status,parent_plan_id) values "
             "(1,'global','M3','m3','draft',null)");

  tss::status report;
  report.summary = tss::plan_summary{
      .anchor_plan_id = 1, .total_tasks = 12345, .tasks_with_slug = 12345, .tasks_covered = 0, .total_scenarios = 0};
  report.milestones.push_back(
      tss::milestone_status{.plan_id = 1, .title = "M3", .total_tasks = 12345, .tasks_with_slug = 12345, .tasks_covered = 0});

  const auto out = tss::render_text("m3", report);
  // The exact captured row -- both surprises in one line: the `+` sign, and
  // the six-character count sitting in a five-wide field, shoving the rest of
  // the row one column right rather than being truncated.
  REQUIRE(out.find("  M3                               +12345 +12345    +0      +0    +0    +0    +0    +0\n") !=
          std::string::npos);
  // Stated as a property too, scoped to the milestone ROW (the summary
  // sentence below it legitimately contains unsigned numbers): every count
  // in the table carries a sign, so the unsigned five-wide form must not
  // appear anywhere on that line.
  const auto row_begin = out.find("  M3 ");
  REQUIRE(row_begin != std::string::npos);
  const auto row = out.substr(row_begin, out.find('\n', row_begin) - row_begin);
  REQUIRE(row.find("    0") == std::string::npos);
  REQUIRE(row.find("12345 ") != std::string::npos);
  REQUIRE(std::ranges::count(row, '+') == 8);
}

TEST_CASE("test_spec_status: long titles truncate to 29 characters plus an ellipsis", "[planning]") {
  tss::status report;
  report.summary = tss::plan_summary{.anchor_plan_id = 1};
  // Exactly 32 characters -- the boundary. Must NOT be truncated.
  report.milestones.push_back(tss::milestone_status{.plan_id = 1, .title = "01234567890123456789012345678901"});
  // 33 characters -- one past. Must become 29 chars + "...".
  report.milestones.push_back(tss::milestone_status{.plan_id = 2, .title = "012345678901234567890123456789012"});

  const auto out = tss::render_text("p", report);
  REQUIRE(out.find("  01234567890123456789012345678901 ") != std::string::npos);
  REQUIRE(out.find("  01234567890123456789012345678... ") != std::string::npos);
}

TEST_CASE("test_spec_status: an empty report still renders a header and a summary line", "[planning]") {
  tss::status report;
  report.summary = tss::plan_summary{.anchor_plan_id = 1};

  // The empty-input shape: header, column row, blank line, summary. No
  // milestone rows, and NOT an empty string.
  const auto out = tss::render_text("p", report);
  REQUIRE(out == "test-spec status for plan 1 (p)\n"
                 "  milestone                        tasks  slug   cov   happy empty error  edge other\n"
                 "\n"
                 "  0 scenarios total; 0 of 0 slug-bearing tasks covered (0 total tasks).\n");

  // ...and the JSON shape is the bare summary line, NOT `[]` and NOT `{}`.
  REQUIRE(tss::render_json(report) ==
          "{\"anchor_plan_id\":1,\"total_tasks\":0,\"tasks_with_slug\":0,\"tasks_covered\":0,\"total_scenarios\":0}\n");
}

TEST_CASE("test_spec_status: the not-found message matches the oracle verbatim", "[planning]") {
  REQUIRE(tss::render_not_found("m1") == "plan 'm1' not found");
  REQUIRE(tss::render_not_found("2") == "plan '2' not found");
  REQUIRE(tss::render_not_found("99") == "plan '99' not found");
  REQUIRE(tss::render_not_found("nosuch") == "plan 'nosuch' not found");
}
