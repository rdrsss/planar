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
import planar.installed_surface;

namespace {

namespace he  = planar::engine::health;
namespace is_ = planar::installed_surface;

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

// ---------------------------------------------------------------------------
// check() / with_projection_freshness() / render_text() — task 6357.
// ---------------------------------------------------------------------------

TEST_CASE("check refuses SchemaTableMissing against a freshly-opened un-migrated DB", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto result = he::check(*conn, "/tmp/test.db");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == he::check_error::schema_table_missing);
}

TEST_CASE("check reports the rich field set after migrations are applied", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            result = he::check(conn, "/tmp/test.db");
  REQUIRE(result.has_value());
  CHECK(result->db_ok);
  CHECK(result->integrity_ok);
  CHECK(result->overall == "ok");
  CHECK(result->inflight_tasks == 0);
  CHECK(result->not_resumable_tasks == 0);
  CHECK(result->schema_current);
  CHECK(result->projection_freshness.state == "not_installed");
  CHECK(result->projection_freshness.unselected_vendors == is_::supported_vendors.size());
}

TEST_CASE("check classifies a doing task with no next_action as not-resumable + degraded", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'doing', 100)");
  auto result = he::check(conn, "/tmp/test.db");
  REQUIRE(result.has_value());
  CHECK(result->inflight_tasks == 1);
  CHECK(result->not_resumable_tasks == 1);
  CHECK(result->overall == "degraded");
}

TEST_CASE("check classifies a doing task WITH a next_action and a snapshot as resumable", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor) values (1, 'claude')");
  exec(conn, "insert into tasks (id, scope_kind, title, status, priority, next_action) values (1, 'global', 'T', 'doing', 100, "
             "'do the thing')");
  exec(conn, "insert into context_snapshots (task_id, session_id, vendor) values (1, 1, 'claude')");
  auto result = he::check(conn, "/tmp/test.db");
  REQUIRE(result.has_value());
  CHECK(result->inflight_tasks == 1);
  CHECK(result->resumable_tasks == 1);
  CHECK(result->not_resumable_tasks == 0);
  CHECK(result->overall == "ok");
}

TEST_CASE("check flags a stale pending handoff as degraded", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor) values (1, 'claude')");
  exec(conn, "insert into context_snapshots (id, session_id, vendor) values (1, 1, 'claude')");
  exec(conn, "insert into handoffs (status, created_at, from_snapshot_id, from_vendor) values ('pending', datetime('now', '-2 "
             "days'), 1, "
             "'claude')");
  auto result = he::check(conn, "/tmp/test.db");
  REQUIRE(result.has_value());
  CHECK(result->pending_handoffs == 1);
  CHECK(result->stale_handoffs == 1);
  CHECK(result->overall == "degraded");
}

TEST_CASE("with_projection_freshness degrades only managed drift and recovery manifest states", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());

  auto current = is_::status(is_::options{.planar_home = "/definitely/not/a/planar/home",
                                          .home        = "/definitely/not/a/home",
                                          .codex_home  = "/definitely/not/a/codex/home"});
  REQUIRE(current.has_value());

  // The classifier itself always names a bootstrap repair command for a
  // missing manifest (see installed_surface's bootstrap_result), so this is
  // the discriminating case for the `degraded ? status.repair_command :
  // nullopt` gate: `current->repair_command` IS set here, but this state is
  // `not_installed`, not `degraded` — the gate must suppress it.
  REQUIRE(current->repair_command.has_value());

  auto with_fresh = he::with_projection_freshness(*base, *current);
  CHECK(with_fresh.projection_freshness.state == "not_installed");
  CHECK(with_fresh.overall == "ok");
  CHECK_FALSE(with_fresh.projection_freshness.repair_command.has_value());
}

TEST_CASE("with_projection_freshness degrades overall on a legacy manifest state", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());

  is_::status_result legacy_status{
      .manifest_status = is_::manifest_state::legacy,
      .manifest_path   = "/tmp/nope/install-manifest.json",
      .reason          = "legacy ownership stamp exists but the versioned install manifest is missing",
      .repair_command  = "./install.sh --prefix '/tmp/nope'",
  };
  legacy_status.summary.unselected_vendors = is_::supported_vendors.size();

  auto degraded = he::with_projection_freshness(*base, legacy_status);
  CHECK(degraded.projection_freshness.state == "degraded");
  CHECK(degraded.overall == "degraded");
  REQUIRE(degraded.projection_freshness.evidence.has_value());
  CHECK(*degraded.projection_freshness.evidence == "legacy ownership stamp exists but the versioned install manifest is missing");
}

// The `manifest_degraded` predicate is a three-way OR (legacy / invalid /
// unsupported), and only `legacy` had engine-level coverage until this
// iteration's review — a mutation dropping `invalid` or `unsupported` from
// that OR-chain survived the whole suite (iteration 2 review finding,
// "generalize the permissive-mutation lens"). Each manifest state that
// SHOULD degrade now gets its own case.

TEST_CASE("with_projection_freshness degrades overall on an invalid manifest state", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());

  is_::status_result invalid_status{
      .manifest_status = is_::manifest_state::invalid,
      .manifest_path   = "/tmp/nope/install-manifest.json",
      .reason          = "install manifest is invalid",
  };
  invalid_status.summary.unselected_vendors = is_::supported_vendors.size();

  auto degraded = he::with_projection_freshness(*base, invalid_status);
  CHECK(degraded.projection_freshness.state == "degraded");
  CHECK(degraded.overall == "degraded");
}

TEST_CASE("with_projection_freshness degrades overall on an unsupported manifest state", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());

  is_::status_result unsupported_status{
      .manifest_status = is_::manifest_state::unsupported,
      .manifest_path   = "/tmp/nope/install-manifest.json",
      .reason          = "install manifest version is unsupported",
  };
  unsupported_status.summary.unselected_vendors = is_::supported_vendors.size();

  auto degraded = he::with_projection_freshness(*base, unsupported_status);
  CHECK(degraded.projection_freshness.state == "degraded");
  CHECK(degraded.overall == "degraded");
}

// `managed_degraded` (`status.summary.stale > 0 || status.summary.missing >
// 0`) is the OTHER disjunct feeding `degraded`, and it is the one
// `planar health` exists to surface: a projection that has drifted or
// vanished from disk with a perfectly CURRENT manifest. Until iteration 3
// review, no fixture anywhere in the tree constructed a `status_result`
// with `stale`/`missing` > 0 and `manifest_status = current` to prove
// `overall` degrades on that path ALONE — every `[cmd][health]` degraded
// case drove degradation through `not_resumable_tasks` instead, so this
// disjunct's own on/off switch was unverified. `stale` and `missing` are
// independent disjuncts (`||`, not `&&`), so each gets its own case rather
// than one fixture asserting both at once.

TEST_CASE("with_projection_freshness degrades overall on stale > 0 alone, with a current manifest", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());

  is_::status_result stale_status{
      .manifest_status = is_::manifest_state::current,
      .manifest_path   = "/tmp/planar-home/install-manifest.json",
      .repair_command  = "./install.sh --prefix '/tmp/planar-home'",
  };
  stale_status.summary.stale = 1;

  auto degraded = he::with_projection_freshness(*base, stale_status);
  CHECK(degraded.projection_freshness.state == "degraded");
  CHECK(degraded.overall == "degraded");
  // `status.reason` is unset here, so this also exercises `evidence`'s
  // FALLBACK arm (`managed_degraded` true, `reason` absent) — previously
  // untested; every other evidence case in this file has `reason` set.
  REQUIRE(degraded.projection_freshness.evidence.has_value());
  CHECK(*degraded.projection_freshness.evidence == "managed projections differ from the staged installation authority");
  // The `degraded ? status.repair_command : nullopt` gate: THIS is the
  // discriminating direction (degraded=true, repair_command SET on the
  // input) — the sibling "not_installed" case above proves the opposite
  // direction (repair_command suppressed when not degraded).
  REQUIRE(degraded.projection_freshness.repair_command.has_value());
  CHECK(*degraded.projection_freshness.repair_command == "./install.sh --prefix '/tmp/planar-home'");
}

TEST_CASE("with_projection_freshness degrades overall on missing > 0 alone, with a current manifest", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());

  is_::status_result missing_status{
      .manifest_status = is_::manifest_state::current,
      .manifest_path   = "/tmp/planar-home/install-manifest.json",
  };
  missing_status.summary.missing = 1;

  auto degraded = he::with_projection_freshness(*base, missing_status);
  CHECK(degraded.projection_freshness.state == "degraded");
  CHECK(degraded.overall == "degraded");
}

TEST_CASE("with_projection_freshness never un-degrades an already-degraded report", "[engine_health]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'doing', 100)");
  auto base = he::check(conn, "/tmp/test.db");
  REQUIRE(base.has_value());
  REQUIRE(base->overall == "degraded");

  is_::status_result fresh_status{.manifest_status = is_::manifest_state::current, .manifest_path = "/tmp/x"};
  fresh_status.summary = is_::summary{.fresh = 1};

  auto result = he::with_projection_freshness(*base, fresh_status);
  CHECK(result.projection_freshness.state == "fresh");
  CHECK(result.overall == "degraded");
}

TEST_CASE("manifest_state_name spells every state exactly as the oracle's enum tag", "[engine_health]") {
  // Only "missing" and "invalid" were ever asserted through render_text
  // before this iteration's review — a mutation swapping any of the other
  // three spellings survived the whole module. Each arm gets its own
  // assertion so a single swapped pair cannot hide behind another.
  CHECK(he::manifest_state_name(is_::manifest_state::current) == "current");
  CHECK(he::manifest_state_name(is_::manifest_state::missing) == "missing");
  CHECK(he::manifest_state_name(is_::manifest_state::legacy) == "legacy");
  CHECK(he::manifest_state_name(is_::manifest_state::invalid) == "invalid");
  CHECK(he::manifest_state_name(is_::manifest_state::unsupported) == "unsupported");
}

TEST_CASE("render_text renders every section including the optional evidence/repair lines", "[engine_health]") {
  he::report report;
  report.db_path              = "/tmp/test.db";
  report.db_ok                = true;
  report.schema_version       = 5;
  report.schema_target        = 5;
  report.schema_current       = true;
  report.migration_count      = 5;
  report.integrity_ok         = true;
  report.inflight_tasks       = 2;
  report.resumable_tasks      = 1;
  report.not_resumable_tasks  = 1;
  report.pending_handoffs     = 1;
  report.stale_handoffs       = 1;
  report.projection_freshness = he::projection_freshness{
      .state              = "degraded",
      .manifest_status    = is_::manifest_state::invalid,
      .managed            = 3,
      .fresh              = 1,
      .stale              = 1,
      .missing            = 1,
      .unmanaged          = 2,
      .unselected_vendors = 1,
      .evidence           = "install manifest is invalid",
      .repair_command     = "./install.sh --prefix '/tmp'",
  };
  report.overall = "degraded";

  CHECK(he::render_text(report) ==
        "db:               ok (/tmp/test.db)\n"
        "schema:           v5 of v5 (current)\n"
        "integrity:        ok\n"
        "in-flight tasks:  2 (1 resumable, 1 NOT resumable)\n"
        "pending handoffs: 1 (1 stale > 24h)\n"
        "projection freshness: degraded (3 managed: 1 fresh, 1 stale, 1 missing; 2 unmanaged; 1 unselected vendors)\n"
        "projection manifest:  invalid\n"
        "projection evidence:  install manifest is invalid\n"
        "projection repair:    ./install.sh --prefix '/tmp'\n"
        "overall:          degraded\n");
}

TEST_CASE("render_text omits the evidence/repair lines when both are unset", "[engine_health]") {
  he::report report;
  report.db_path                                 = "/tmp/test.db";
  report.schema_current                          = true;
  report.integrity_ok                            = true;
  report.overall                                 = "ok";
  report.projection_freshness.state              = "not_installed";
  report.projection_freshness.manifest_status    = is_::manifest_state::missing;
  report.projection_freshness.unselected_vendors = is_::supported_vendors.size();
  report.projection_freshness.evidence           = std::nullopt;
  report.projection_freshness.repair_command     = std::nullopt;

  auto const text = he::render_text(report);
  CHECK(text.find("projection evidence:") == std::string::npos);
  CHECK(text.find("projection repair:") == std::string::npos);
  CHECK(text.find("projection manifest:  missing\n") != std::string::npos);
}
