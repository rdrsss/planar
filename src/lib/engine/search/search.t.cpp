// @file search.t.cpp
// @brief Unit tests for `planar.engine.search` (plan 996, task 6090).
//
// The leaf-level bytes are pinned in
// `src/cmd/planar/search_health_audit_leaves.t.cpp`. What is pinned HERE
// is the query composition itself — the properties a caller other than
// `planar search` would depend on, and the ones a rewrite of the
// `UNION ALL` builder would break silently:
//
//   * EMPTY `kinds` means ALL SIX, not none. The single most plausible
//     off-by-one in this module, and it fails closed in the wrong
//     direction: an implementation that read empty as "no arms" would
//     return no rows, which is indistinguishable from a genuine no-match.
//   * An unrecognised kind REFUSES rather than contributing no arm.
//   * `--plan` filters `task` by COLUMN and the other five by
//     `entity_links` EXISTS. Asserted with a fixture that has both shapes
//     present, so an implementation that used one rule for all six fails.
//   * `limit` caps the MERGED union, not each arm.
//   * `rank` is NEGATED bm25, so a higher number is more relevant — the
//     sign is what makes `order by rank desc` mean what it says.
//
// FIXTURE NON-EMPTINESS IS ASSERTED, not assumed: `search` is a
// result-set function and a fixture that stored nothing makes every case
// below pass against the empty answer.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.search;

namespace {

namespace se = planar::engine::search;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_search_test_{}_{}.db",
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

/// @brief Seed one row of every searchable kind, all carrying the token
/// `zephyr`.
///
/// Also creates a plan the `--plan` cases filter on, plus a `derives-from`
/// edge from the DECISION to it — so the fixture carries both halves of the
/// column-vs-link asymmetry and a test can tell them apart.
void seed(planar::db::connection& conn) {
  exec(conn, "insert into plans (scope_kind, title, slug, summary, status) "
             "values ('global', 'Zephyr plan', 'zephyr-plan', 'zephyr summary', 'active')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, body, status, priority) "
             "values ('global', 1, 'Zephyr task', 'zephyr body', 'todo', 100)");
  exec(conn, "insert into questions (scope_kind, title, body, status) "
             "values ('global', 'Zephyr question', 'zephyr body', 'open')");
  exec(conn, "insert into test_scenarios (scope_kind, title, body, status) "
             "values ('global', 'Zephyr scenario', 'zephyr body', 'draft')");
  exec(conn, "insert into decisions (scope_kind, title, body, status) "
             "values ('global', 'Zephyr decision', 'zephyr body', 'proposed')");
  exec(conn, "insert into artifacts (scope_kind, title, body, kind, status) "
             "values ('global', 'Zephyr artifact', 'zephyr body', 'other', 'draft')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('decision', 1, 'plan', 1, 'derives-from')");
}

auto kinds_of(std::span<const se::hit> hits) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& h : hits) {
    out.push_back(h.kind);
  }
  return out;
}

} // namespace

TEST_CASE("an EMPTY kind filter searches all six kinds", "[engine][search]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto hits = se::query(conn, "zephyr", se::search_filter{});
  REQUIRE(hits.has_value());
  // THE VACUITY GUARD, and the central case of this file. Six, not zero:
  // an implementation reading "no kinds requested" as "emit no arms" would
  // return an empty vector here and pass any assertion phrased as
  // "succeeds".
  REQUIRE(hits->size() == 6);
  // Ranks tie across all six, so the visible order is entirely the
  // `kind ASC, id ASC` tie-break.
  CHECK(kinds_of(*hits) == std::vector<std::string>{"artifact", "decision", "plan", "question", "scenario", "task"});
  CHECK((*hits)[0].snippet.find("<mark>") != std::string::npos);
}

TEST_CASE("an explicit kind list narrows to exactly those arms", "[engine][search]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  se::search_filter one;
  one.kinds  = {"plan"};
  auto plans = se::query(conn, "zephyr", one);
  REQUIRE(plans.has_value());
  REQUIRE(plans->size() == 1);
  CHECK((*plans)[0].kind == "plan");
  CHECK((*plans)[0].slug == "zephyr-plan");
  CHECK((*plans)[0].status == "active");

  se::search_filter two;
  two.kinds  = {"plan", "task"};
  auto pair_ = se::query(conn, "zephyr", two);
  REQUIRE(pair_.has_value());
  CHECK(kinds_of(*pair_) == std::vector<std::string>{"plan", "task"});
}

TEST_CASE("an unrecognised kind REFUSES instead of contributing no arm", "[engine][search]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  se::search_filter bad;
  bad.kinds = {"plan", "bogus"};
  auto out  = se::query(conn, "zephyr", bad);
  REQUIRE(!out.has_value());
  CHECK(out.error() == se::search_error::unknown_kind);
  // The refusal is the point: dropping the arm would have returned the one
  // real `plan` hit, which reads as a working filter.
}

TEST_CASE("rank is NEGATED bm25, so more relevant is a LARGER number", "[engine][search][rank]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  // Two plans, one where the term is the whole document and one where it is
  // buried in filler. bm25 favours the short one, and after negation that
  // must come out FIRST with the LARGER rank.
  exec(conn, "insert into plans (scope_kind, title, slug, summary, status) "
             "values ('global', 'zephyr', 'short-one', 'zephyr', 'active')");
  exec(conn, "insert into plans (scope_kind, title, slug, summary, status) "
             "values ('global', 'zephyr', 'long-one', 'zephyr alpha beta gamma delta epsilon zeta eta theta iota kappa "
             "lambda mu nu xi omicron pi rho sigma tau upsilon', 'active')");

  se::search_filter only_plans;
  only_plans.kinds = {"plan"};
  auto hits        = se::query(conn, "zephyr", only_plans);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0].slug == "short-one");
  // Strictly greater, and both positive. An unnegated bm25 would put the
  // long document first AND make both values negative, so this one
  // assertion catches a dropped minus sign in two independent ways.
  CHECK((*hits)[0].rank > (*hits)[1].rank);
  CHECK((*hits)[1].rank > 0.0);
}

// The title must NOT begin with `--`: `catch_discover_tests` hands it back
// as argv and Catch2 parses a leading `--plan …` as a flag, so the case
// goes red under ctest while passing by tag. It did exactly that here
// before being renamed.
TEST_CASE("the plan filter uses tasks' COLUMN and other kinds' entity_links", "[engine][search][plan]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  se::search_filter by_plan;
  by_plan.plan_id = 1;
  auto hits       = se::query(conn, "zephyr", by_plan);
  REQUIRE(hits.has_value());
  // The task matches through its `plan_id` COLUMN; the decision matches
  // through its `derives-from` EDGE. The other four have neither, and the
  // PLAN ITSELF does not match its own id — a plan does not derive from
  // itself. All three facts are in this one expectation, and a uniform
  // rule for all six kinds cannot produce it.
  CHECK(kinds_of(*hits) == std::vector<std::string>{"decision", "task"});
}

TEST_CASE("limit caps the MERGED union rather than each arm", "[engine][search][limit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  se::search_filter capped;
  capped.limit = 2;
  auto hits    = se::query(conn, "zephyr", capped);
  REQUIRE(hits.has_value());
  // Two TOTAL. A per-arm cap would return up to twelve here (six arms x
  // two), so the number distinguishes the two placements outright.
  CHECK(hits->size() == 2);

  se::search_filter zero;
  zero.limit  = 0;
  auto unset_ = se::query(conn, "zephyr", zero);
  REQUIRE(unset_.has_value());
  // Zero is the DEFAULT, not "none".
  CHECK(unset_->size() == 6);

  se::search_filter negative;
  negative.limit = -5;
  auto neg       = se::query(conn, "zephyr", negative);
  REQUIRE(neg.has_value());
  CHECK(neg->size() == 6);
}

TEST_CASE("an unresolvable scope slug refuses rather than matching nothing", "[engine][search][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  se::search_filter bad;
  bad.scope = "nosuchscope";
  auto out  = se::query(conn, "zephyr", bad);
  REQUIRE(!out.has_value());
  CHECK(out.error() == se::search_error::slug_not_found);

  // `global` DOES resolve, and every seeded row is global — which is what
  // makes the refusal above evidence about the slug rather than about the
  // scope predicate rejecting everything.
  se::search_filter global;
  global.scope = "global";
  auto all     = se::query(conn, "zephyr", global);
  REQUIRE(all.has_value());
  CHECK(all->size() == 6);
}

TEST_CASE("FTS5 syntax errors surface as invalid_query, not as no results", "[engine][search][fts]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto bad = se::query(conn, "\"unclosed", se::search_filter{});
  REQUIRE(!bad.has_value());
  CHECK(bad.error() == se::search_error::invalid_query);

  // A well-formed query that simply matches nothing is SUCCESS with an
  // empty vector. The contrast is the contract: a caller must be able to
  // tell "you typed nonsense" from "nothing matched".
  auto none = se::query(conn, "qqqzzznomatch", se::search_filter{});
  REQUIRE(none.has_value());
  CHECK(none->empty());
}

TEST_CASE("a status filter narrows, and an EMPTY status string matches nothing", "[engine][search][status]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  se::search_filter todo;
  todo.statuses = {"todo"};
  auto tasks    = se::query(conn, "zephyr", todo);
  REQUIRE(tasks.has_value());
  CHECK(kinds_of(*tasks) == std::vector<std::string>{"task"});

  // The empty string is a VALUE here, not an absent filter — `status IN
  // ('')` is empty. The leaf above relies on this; see
  // handlers/search.cppm.
  se::search_filter empty_value;
  empty_value.statuses = {""};
  auto nothing         = se::query(conn, "zephyr", empty_value);
  REQUIRE(nothing.has_value());
  CHECK(nothing->empty());

  // An empty LIST, by contrast, means no status predicate at all.
  se::search_filter no_filter;
  auto              everything = se::query(conn, "zephyr", no_filter);
  REQUIRE(everything.has_value());
  CHECK(everything->size() == 6);
}

TEST_CASE("render_list_text prints (no results) for an empty list", "[engine][search][render]") {
  CHECK(se::render_list_text({}) == "(no results)\n");
}

TEST_CASE("render_list_text shows the slug and status only when present", "[engine][search][render]") {
  std::vector<se::hit> hits{
      se::hit{.kind = "plan", .id = 7, .slug = "my-plan", .title = "A plan", .snippet = "<mark>A</mark> plan", .status = "draft"},
      se::hit{.kind = "task", .id = 3, .slug = "", .title = "A task", .snippet = "", .status = ""},
  };
  // Four independent conditionals in two lines: slug present/absent and
  // status present/absent, plus the snippet line appearing only when the
  // snippet is non-empty. The second hit exercises all three "absent"
  // arms at once, which is the row shape a task actually has.
  CHECK(se::render_list_text(hits) == "plan:7 (my-plan) [draft] — A plan\n"
                                      "  <mark>A</mark> plan\n"
                                      "task:3 — A task\n");
}

TEST_CASE("render_list_text falls back to the bare kind on an over-long reference", "[engine][search][render]") {
  // The oracle formats the reference into a fixed [256]u8 and prints just
  // `h.kind` on overflow, losing the id. Reproduced rather than fixed, so
  // it is pinned rather than left to be rediscovered as a bug.
  //
  // The slug length is written as a LITERAL rather than derived from the
  // 256 constant on purpose: deriving it would mean widening the constant
  // also widens the test, and the case would survive the change it exists
  // to catch.
  std::vector<se::hit> over{
      se::hit{.kind = "plan", .id = 7, .slug = std::string(300, 'q'), .title = "A plan", .snippet = "", .status = "draft"},
  };
  CHECK(se::render_list_text(over) == "plan [draft] — A plan\n");

  // A reference comfortably UNDER the cap keeps its id, which is what
  // makes the case above evidence about the cap rather than about slugs.
  std::vector<se::hit> under{
      se::hit{.kind = "plan", .id = 7, .slug = std::string(10, 'q'), .title = "A plan", .snippet = "", .status = "draft"},
  };
  CHECK(se::render_list_text(under) == std::format("plan:7 ({}) [draft] — A plan\n", std::string(10, 'q')));
}
