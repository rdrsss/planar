// @file health.t.cpp
// @brief Unit tests for `planar.engine.health` (plan 996, task 6090).
//
// The three age-boundary cases here are the reason `hygiene_options::now`
// exists. The thresholds compare `julianday(coalesce(?, 'now'))` against
// each row's `updated_at`, so pinning the LEFT side lets a test place a row
// exactly ON a boundary — the only shape that can tell `>` from `>=` — and
// makes the whole file deterministic without sleeping.
//
// What is pinned:
//
//   * The doing/open comparisons are STRICT. A row exactly `threshold` days
//     old does NOT appear; one day older does. Both are asserted from the
//     SAME fixture, so `>=` fails and `>` passes rather than both passing.
//   * `age_days` TRUNCATES. A row 7.9 days old reports 7.
//   * A draft plan's staleness is STRUCTURAL, not temporal: a brand-new
//     empty draft is stale immediately, and a plan with one live task never
//     is, however old.
//   * `reason` and the suggested status are driven by the SAME test, so
//     `zero_tasks` always pairs with `abandoned` and `all_tasks_terminal`
//     with `done`.
//   * `--scope` must name an ASSOCIATION. `global` and a repo ref both
//     refuse, and they refuse DIFFERENTLY from an unknown slug.
//
// Rows are inserted with raw SQL because the engine takes no writes and the
// cases need control over `updated_at`, which no verb exposes.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.health;

namespace {

namespace he = planar::engine::health;

/// @brief The fixed clock every dated case measures from.
constexpr std::string_view k_now = "2026-06-01T00:00:00.000Z";

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_health_test_{}_{}.db",
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

auto at_fixed_clock() -> he::hygiene_options {
  return he::hygiene_options{.scope = std::nullopt, .stale_doing_days = 7, .stale_open_days = 30, .now = std::string{k_now}};
}

} // namespace

TEST_CASE("a draft plan with zero tasks is stale immediately and suggests abandoned", "[engine][health][hygiene]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'Empty', 'empty', 'draft')");
  // A live task keeps its plan OFF the list no matter how old it is: the
  // draft-plan rule is structural, so this row is dated far in the past and
  // still must not appear.
  exec(conn, "insert into plans (scope_kind, title, slug, status, updated_at) "
             "values ('global', 'Working', 'working', 'draft', '2020-01-01T00:00:00.000Z')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 2, 'live', 'todo', 100)");

  auto report = he::hygiene(conn, at_fixed_clock());
  REQUIRE(report.has_value());
  REQUIRE(report->stale_draft_plans.size() == 1);
  auto const& row = report->stale_draft_plans[0];
  CHECK(row.id == 1);
  CHECK(row.title == "Empty");
  CHECK(row.reason == "zero_tasks");
  CHECK(row.suggestion == "planar plan update 1 --status abandoned");
  CHECK(!row.parent_plan_id.has_value());
  CHECK(row.counts.todo == 0);
  CHECK(row.counts.done == 0);
}

TEST_CASE("a draft plan whose tasks are all terminal suggests done, not abandoned", "[engine][health][hygiene]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'Finished', 'finished', 'draft')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 'a', 'done', 100)");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 'b', 'cancelled', 100)");

  auto report = he::hygiene(conn, at_fixed_clock());
  REQUIRE(report.has_value());
  REQUIRE(report->stale_draft_plans.size() == 1);
  auto const& row = report->stale_draft_plans[0];
  // `reason` and the suggested STATUS are driven by the same `task_count ==
  // 0` test, so they are asserted together: a port where they disagreed
  // would tell an operator to abandon a plan whose work is finished.
  CHECK(row.reason == "all_tasks_terminal");
  CHECK(row.suggestion == "planar plan update 1 --status done");
  CHECK(row.counts.done == 1);
  CHECK(row.counts.cancelled == 1);
  CHECK(row.counts.todo == 0);

  // One live task removes it entirely — the boundary of "all terminal".
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 'c', 'blocked', 100)");
  auto after = he::hygiene(conn, at_fixed_clock());
  REQUIRE(after.has_value());
  CHECK(after->stale_draft_plans.empty());
}

TEST_CASE("a non-draft plan never appears however empty or old", "[engine][health][hygiene]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into plans (scope_kind, title, slug, status, updated_at) "
             "values ('global', 'Active empty', 'active-empty', 'active', '2020-01-01T00:00:00.000Z')");

  auto report = he::hygiene(conn, at_fixed_clock());
  REQUIRE(report.has_value());
  CHECK(report->stale_draft_plans.empty());
}

TEST_CASE("the doing-task age comparison is STRICT and age_days truncates", "[engine][health][hygiene][age]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')");
  // EXACTLY seven days before the fixed clock: on the boundary.
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority, updated_at) "
             "values ('global', 1, 'exactly seven', 'doing', 100, '2026-05-25T00:00:00.000Z')");
  // Seven days and 21.6 hours: over the boundary, and a fractional age so
  // the truncation is observable.
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority, updated_at) "
             "values ('global', 1, 'seven and nine tenths', 'doing', 100, '2026-05-24T02:24:00.000Z')");
  // A `todo` task at the same age must not appear: the filter is on status
  // as well as age.
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority, updated_at) "
             "values ('global', 1, 'old but todo', 'todo', 100, '2020-01-01T00:00:00.000Z')");

  auto report = he::hygiene(conn, at_fixed_clock());
  REQUIRE(report.has_value());
  // ONE, not two. The exactly-seven row is what separates `>` from `>=`,
  // and it is in the same fixture as the row that DOES qualify, so neither
  // comparison passes both assertions.
  REQUIRE(report->stale_doing_tasks.size() == 1);
  auto const& row = report->stale_doing_tasks[0];
  CHECK(row.title == "seven and nine tenths");
  // 7.9 days truncates to 7, it does not round to 8.
  CHECK(row.age_days == 7);
  CHECK(row.plan_id == 1);
  CHECK(row.scope == "global");
  CHECK(row.suggestion == "planar task update 2 --scope global --status done OR "
                          "planar task update 2 --scope global --status blocked");
}

TEST_CASE("the open-question age comparison is STRICT too", "[engine][health][hygiene][age]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Exactly thirty days: on the boundary, excluded.
  exec(conn, "insert into questions (scope_kind, title, status, updated_at) "
             "values ('global', 'exactly thirty', 'open', '2026-05-02T00:00:00.000Z')");
  // Forty-five days: included.
  exec(conn, "insert into questions (scope_kind, title, status, updated_at) "
             "values ('global', 'forty-five', 'open', '2026-04-17T00:00:00.000Z')");
  // Answered at the same age: excluded by status, not by age.
  //
  // `answer_body` and `answered_at` are supplied because `questions`
  // carries a CHECK tying them to the `answered` status — an INSERT
  // without them fails, which is how the first draft of this fixture found
  // out. Leaving them off would have made this row absent for the wrong
  // reason and the status filter untested.
  exec(conn, "insert into questions (scope_kind, title, status, answer_body, answered_at, updated_at) "
             "values ('global', 'answered and ancient', 'answered', 'because', "
             "'2020-01-01T00:00:00.000Z', '2020-01-01T00:00:00.000Z')");

  auto report = he::hygiene(conn, at_fixed_clock());
  REQUIRE(report.has_value());
  REQUIRE(report->stale_open_questions.size() == 1);
  CHECK(report->stale_open_questions[0].title == "forty-five");
  CHECK(report->stale_open_questions[0].age_days == 45);
  CHECK(report->stale_open_questions[0].suggestion ==
        "planar question answer 2 --answer \"<resolution>\" OR planar question wontfix 2");
}

TEST_CASE("a zero threshold admits anything strictly older than today", "[engine][health][hygiene][age]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority, updated_at) "
             "values ('global', 1, 'yesterday', 'doing', 100, '2026-05-31T00:00:00.000Z')");
  // Zero is a VALID threshold, distinct from negative (which refuses).
  auto options             = at_fixed_clock();
  options.stale_doing_days = 0;
  auto report              = he::hygiene(conn, options);
  REQUIRE(report.has_value());
  CHECK(report->stale_doing_tasks.size() == 1);
  CHECK(report->thresholds.stale_doing_days == 0);
}

TEST_CASE("a negative threshold refuses on either flag", "[engine][health][hygiene][refusal]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto doing             = at_fixed_clock();
  doing.stale_doing_days = -1;
  auto a                 = he::hygiene(conn, doing);
  REQUIRE(!a.has_value());
  CHECK(a.error() == he::hygiene_error::invalid_threshold);

  // Both flags, separately — a guard that checked only the first would
  // pass a test that exercised only the first.
  auto open            = at_fixed_clock();
  open.stale_open_days = -1;
  auto b               = he::hygiene(conn, open);
  REQUIRE(!b.has_value());
  CHECK(b.error() == he::hygiene_error::invalid_threshold);
}

TEST_CASE("scope must name an association, and the two refusals differ", "[engine][health][hygiene][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into associations (slug, name, kind) values ('proj', 'Proj', 'project')");
  exec(conn, "insert into projects (slug, name, root_path) values ('repo', 'Repo', '/tmp/repo')");
  exec(conn,
       "insert into plans (scope_kind, scope_id, title, slug, status) values ('association', 1, 'Empty', 'empty', 'draft')");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'Global empty', 'gempty', 'draft')");

  // The association scope narrows to ONE of the two drafts. That it
  // narrows rather than returning nothing is what makes the refusals below
  // evidence about the KIND check rather than about an empty database.
  auto options  = at_fixed_clock();
  options.scope = "proj";
  auto scoped   = he::hygiene(conn, options);
  REQUIRE(scoped.has_value());
  REQUIRE(scoped->stale_draft_plans.size() == 1);
  CHECK(scoped->stale_draft_plans[0].title == "Empty");

  // A repo ref RESOLVES but is the wrong kind: unsupported_scope.
  options.scope = "repo:repo";
  auto repo     = he::hygiene(conn, options);
  REQUIRE(!repo.has_value());
  CHECK(repo.error() == he::hygiene_error::unsupported_scope);

  // `global` likewise resolves and is likewise refused.
  options.scope = "global";
  auto global   = he::hygiene(conn, options);
  REQUIRE(!global.has_value());
  CHECK(global.error() == he::hygiene_error::unsupported_scope);

  // An unknown slug does NOT resolve, and gets a DIFFERENT error. The leaf
  // above maps the two to different messages.
  options.scope = "nosuch";
  auto missing  = he::hygiene(conn, options);
  REQUIRE(!missing.has_value());
  CHECK(missing.error() == he::hygiene_error::slug_not_found);
}

TEST_CASE("an unscoped report spans every scope", "[engine][health][hygiene][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into associations (slug, name, kind) values ('proj', 'Proj', 'project')");
  exec(conn, "insert into plans (scope_kind, scope_id, title, slug, status) values ('association', 1, 'Assoc', 'a', 'draft')");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'Global', 'g', 'draft')");

  // No scope predicate at all — the opposite of what an absent `--scope`
  // means on the listing verbs, which narrow to the cwd read set.
  auto report = he::hygiene(conn, at_fixed_clock());
  REQUIRE(report.has_value());
  REQUIRE(report->stale_draft_plans.size() == 2);
}

TEST_CASE("render_hygiene_text always prints all three sections", "[engine][health][hygiene][render]") {
  he::hygiene_report empty;
  // An empty report is STRUCTURE, not zero bytes: an operator needs to see
  // that all three checks ran and found nothing.
  CHECK(render_hygiene_text(empty) == "=== Stale draft plans (all tasks terminal) ===\n"
                                      "  none\n"
                                      "\n=== Stale doing tasks (status=doing for >7 days) ===\n"
                                      "  none\n"
                                      "\n=== Stale open questions (status=open for >30 days) ===\n"
                                      "  none\n");
}

TEST_CASE("render_hygiene_text names the parent plan only when there is one", "[engine][health][hygiene][render]") {
  he::hygiene_report report;
  report.thresholds = {.stale_doing_days = 3, .stale_open_days = 9};
  report.stale_draft_plans.push_back(he::stale_draft_plan{.id             = 4,
                                                          .parent_plan_id = 2,
                                                          .title          = "Nested",
                                                          .reason         = "all_tasks_terminal",
                                                          .counts         = {.done = 2},
                                                          .suggestion     = "planar plan update 4 --status done"});
  report.stale_draft_plans.push_back(he::stale_draft_plan{.id             = 5,
                                                          .parent_plan_id = std::nullopt,
                                                          .title          = "Top level",
                                                          .reason         = "zero_tasks",
                                                          .counts         = {},
                                                          .suggestion     = "planar plan update 5 --status abandoned"});
  report.stale_doing_tasks.push_back(he::stale_doing_task{
      .id = 8, .plan_id = 4, .scope = "global", .title = "Stuck", .age_days = 11, .suggestion = "do a thing"});
  report.stale_open_questions.push_back(
      he::stale_open_question{.id = 3, .title = "Why", .age_days = 40, .suggestion = "answer it"});

  // The whole payload, including the two thresholds interpolated into the
  // section headers and the reason-dependent parenthetical on each plan.
  CHECK(render_hygiene_text(report) == "=== Stale draft plans (all tasks terminal) ===\n"
                                       "  plan 4 (parent: plan:2): \"Nested\"\n"
                                       "    tasks: 0 todo, 0 doing, 0 blocked, 2 done, 0 cancelled\n"
                                       "    suggest: planar plan update 4 --status done  (all tasks terminal)\n"
                                       "  plan 5: \"Top level\"\n"
                                       "    tasks: 0 todo, 0 doing, 0 blocked, 0 done, 0 cancelled\n"
                                       "    suggest: planar plan update 5 --status abandoned  (no tasks ever attached)\n"
                                       "\n=== Stale doing tasks (status=doing for >3 days) ===\n"
                                       "  task 8 (plan 4, last touched 11d ago): \"Stuck\"\n"
                                       "    suggest: do a thing\n"
                                       "\n=== Stale open questions (status=open for >9 days) ===\n"
                                       "  question 3 (40d old): \"Why\"\n"
                                       "    suggest: answer it\n");
}
