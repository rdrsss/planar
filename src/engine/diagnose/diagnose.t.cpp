// @file diagnose.t.cpp
// @brief Unit tests for `planar.engine.diagnose` (plan 1132, task 7372).
//
// The engine ships with an empty catalog, so every case drives it with a
// synthetic catalog whose checks read `tasks`. What is pinned:
//
//   * Window and scope: `--plan` defaults to the plan's lifetime, `--days`
//     overrides it, a task attached to the anchor and one in a descendant
//     (grandchild included) are in scope, an unrelated plan and a plan-less
//     task are not; without a plan the window is `--days`, default 7.
//   * The evaluation instant is injected: it is echoed (normalised to
//     millisecond precision) and is the window's end.
//   * An input needed only by an unselected or unbuilt check is
//     `not_applicable` and its probe never runs; an input a selected, built
//     check needs and cannot read makes the outcome `partial`.
//   * Findings come back in the fixed order, in text and in JSON.
//   * A lock held past 250 ms ends the run `unavailable (busy)`, and the
//     connection's previous busy timeout is restored on every path.
//   * Bad input is a `run_error`, never an `unavailable` diagnosis.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.diagnose;
import planar.incident_model;
import planar.json_dom;

namespace {

namespace dg = planar::engine::diagnose;
namespace im = planar::incident_model;

constexpr std::string_view k_now = "2026-06-01T00:00:00.000Z";

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_diagnose_test_{}_{}.db",
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

auto add_plan(planar::db::connection& conn, int id, std::optional<int> parent, std::string_view created) -> void {
  exec(conn, std::format("insert into plans (id, scope_kind, title, slug, parent_plan_id, created_at) values ({}, 'global', "
                         "'p{}', 'p{}', {}, '{}')",
                         id, id, id, parent ? std::to_string(*parent) : std::string{"null"}, created));
}

auto add_task(planar::db::connection& conn, int id, std::optional<int> plan, int priority, std::string_view created) -> void {
  exec(conn,
       std::format(
           "insert into tasks (id, scope_kind, plan_id, title, priority, created_at) values ({}, 'global', {}, 't{}', {}, '{}')",
           id, plan ? std::to_string(*plan) : std::string{"null"}, id, priority, created));
}

/// A synthetic check: one finding per task created inside the window and the plan scope. Priority
/// 3 reports an error, 2 a warning, anything else info.
auto task_check(std::string id, std::vector<std::string> inputs = {}, bool built = true) -> dg::check_def {
  dg::check_def def;
  def.id       = std::move(id);
  def.kind     = im::check_kind::event;
  def.severity = im::diagnostic_severity::error;
  def.category = "test_category";
  def.recovery = "do the thing";
  def.inputs   = std::move(inputs);
  def.built    = built;
  if (!built) {
    return def;
  }
  def.evaluate = [](const dg::check_context& ctx) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    auto stmt = ctx.conn.prepare(
        std::format("select id, priority, created_at from tasks where {} and created_at >= ?1 and created_at <= ?2 order by id",
                    dg::plan_filter_sql(ctx.scope, "plan_id")));
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    if (auto ok = stmt->bind_text(1, ctx.window.from); !ok) {
      return std::unexpected(ok.error());
    }
    if (auto ok = stmt->bind_text(2, ctx.window.to); !ok) {
      return std::unexpected(ok.error());
    }
    std::vector<im::finding> out;
    while (true) {
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(step.error());
      }
      if (step.value() == planar::db::step_result::done) {
        break;
      }
      im::finding f;
      auto        prio = stmt->column_int64(1);
      f.severity       = prio >= 3 ? im::diagnostic_severity::error
                                   : (prio == 2 ? im::diagnostic_severity::warning : im::diagnostic_severity::info);
      f.primary        = im::entity_ref{.kind = "task", .id = stmt->column_int64(0)};
      f.evidence       = {f.primary};
      f.evidence_times = {stmt->column_text(2)};
      out.push_back(std::move(f));
    }
    return out;
  };
  return def;
}

auto catalog_of(std::vector<dg::check_def> checks, std::vector<dg::input_def> inputs = {}) -> dg::catalog {
  return dg::catalog{.inputs = std::move(inputs), .checks = std::move(checks)};
}

auto request(std::optional<int> plan, std::optional<int> days = std::nullopt, std::string at = std::string{k_now})
    -> dg::run_request {
  return dg::run_request{.plan_id = plan, .days = days, .checks = {}, .evaluated_at = std::move(at)};
}

auto task_ids(const dg::diagnosis& d) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> ids;
  for (const auto& f : d.findings) {
    ids.push_back(f.primary.id);
  }
  std::ranges::sort(ids);
  return ids;
}

auto busy_timeout_of(planar::db::connection& conn) -> std::int64_t {
  auto stmt = conn.prepare("pragma busy_timeout");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  return stmt->column_int64(0);
}

/// Anchor 1 (created 40 days before `k_now`) with child 2 and grandchild 3, an unrelated plan 9. Tasks: 11 on the
/// anchor at day 2, 12 in the child at day 39, 13 in the grandchild at day 39, 14 on the unrelated plan at day 39, 15
/// with no plan at day 39.
auto build_forty_day_fixture(planar::db::connection& conn) -> void {
  add_plan(conn, 1, std::nullopt, "2026-04-22T00:00:00.000Z");
  add_plan(conn, 2, 1, "2026-04-23T00:00:00.000Z");
  add_plan(conn, 3, 2, "2026-04-24T00:00:00.000Z");
  add_plan(conn, 9, std::nullopt, "2026-04-22T00:00:00.000Z");
  add_task(conn, 11, 1, 2, "2026-04-24T00:00:00.000Z");
  add_task(conn, 12, 2, 2, "2026-05-31T00:00:00.000Z");
  add_task(conn, 13, 3, 2, "2026-05-31T00:00:00.000Z");
  add_task(conn, 14, 9, 2, "2026-05-31T00:00:00.000Z");
  add_task(conn, 15, std::nullopt, 2, "2026-05-31T00:00:00.000Z");
}

} // namespace

TEST_CASE("the shipped catalog reads clean over an empty database and keeps unbuilt checks unbuilt", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            cat  = dg::builtin_catalog();
  auto            it   = std::ranges::find(cat.checks, "queue-ended-unobserved", &dg::check_def::id);
  REQUIRE(it != cat.checks.end());
  CHECK(it->kind == im::check_kind::state);
  CHECK(it->severity == im::diagnostic_severity::warning);
  CHECK(it->category == "queue_unobserved");
  CHECK_FALSE(it->built);

  auto d = dg::run(conn, request(std::nullopt));
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::ok);
  CHECK(d->findings.empty());
  CHECK(d->catalog_version == dg::k_catalog_version);
  REQUIRE(d->checks.size() == cat.checks.size());
  auto summary = std::ranges::find(d->checks, "queue-ended-unobserved", &dg::check_summary::id);
  REQUIRE(summary != d->checks.end());
  CHECK(summary->state == dg::check_state::not_built);
  for (std::string_view name : {"run_identity", "queue_observation"}) {
    auto row = std::ranges::find(d->coverage, name, &im::coverage_row::input);
    REQUIRE(row != d->coverage.end());
    CHECK(row->state == im::coverage_state::not_applicable);
    CHECK(row->reason == "check-not-built");
  }
}

TEST_CASE("a finding's primary entity is always part of its evidence", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  dg::check_def   def;
  def.id       = "forgetful";
  def.evaluate = [](const dg::check_context&) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    im::finding missing;
    missing.primary  = im::entity_ref{.kind = "claim", .id = 7};
    missing.evidence = {im::entity_ref{.kind = "task", .id = 3}};
    im::finding present;
    present.primary  = im::entity_ref{.kind = "claim", .id = 8};
    present.evidence = {present.primary};
    return std::vector<im::finding>{missing, present};
  };
  auto d = dg::run(conn, request(std::nullopt), catalog_of({def}));
  REQUIRE(d.has_value());
  REQUIRE(d->findings.size() == 2);
  CHECK(im::finding_fingerprint(d->findings[0]) == "forgetful|claim:7,task:3");
  CHECK(d->findings[0].evidence.size() == 2);
  CHECK(im::finding_fingerprint(d->findings[1]) == "forgetful|claim:8");
  CHECK(d->findings[1].evidence.size() == 1);
}

TEST_CASE("a cluster finding prints its grouping fingerprint in text and JSON", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  dg::check_def   def;
  def.id       = "claim-failure-cluster";
  def.evaluate = [](const dg::check_context&) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    im::finding f;
    f.primary  = im::entity_ref{.kind = "claim", .id = 1};
    f.evidence = {f.primary, im::entity_ref{.kind = "claim", .id = 2}, im::entity_ref{.kind = "claim", .id = 3}};
    f.group    = im::grouping{.key_parts = {"failure_category=tool_failure"}, .scope = "global"};
    f.members  = {{f.evidence[0], "2026-05-01T00:00:00.000Z"}};
    return std::vector<im::finding>{f};
  };
  auto d = dg::run(conn, request(std::nullopt), catalog_of({def}));
  REQUIRE(d.has_value());
  auto parsed = planar::json_dom::parse_json(dg::render_json(*d));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->find("findings")->array.size() == 1);
  CHECK(parsed->find("findings")->array[0].find("fingerprint")->string ==
        "claim-failure-cluster|failure_category=tool_failure|global");
}

TEST_CASE("a run is read-only on a read-write connection and restores query_only", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_plan(conn, 1, std::nullopt, "2026-05-01T00:00:00.000Z");
  dg::check_def def;
  def.id       = "writer";
  def.evaluate = [](const dg::check_context& ctx) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    auto wrote = ctx.conn.execute("update plans set title = 'changed'");
    if (!wrote) {
      return std::unexpected(wrote.error());
    }
    return std::vector<im::finding>{};
  };
  auto d = dg::run(conn, request(std::nullopt), catalog_of({def}));
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::unavailable);
  CHECK(d->reason == dg::unavailable_reason::query_failed);
  auto stmt = conn.prepare("select title from plans where id = 1");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_text(0) == "p1");
  // The connection can write again afterwards.
  exec(conn, "update plans set title = 'later' where id = 1");
}

TEST_CASE("a plan scope defaults to the plan lifetime and --days overrides it", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  build_forty_day_fixture(conn);
  auto cat = catalog_of({task_check("task-seen")});

  SECTION("plan lifetime reports the day-2 and the day-39 task, descendants included") {
    auto d = dg::run(conn, request(1), cat);
    REQUIRE(d.has_value());
    CHECK(d->window.source == dg::window_source::plan_lifetime);
    CHECK(d->window.from == "2026-04-22T00:00:00.000Z");
    CHECK(d->window.to == k_now);
    CHECK(d->scope.plan_ids == std::vector<std::int64_t>{1, 2, 3});
    CHECK(task_ids(*d) == std::vector<std::int64_t>{11, 12, 13});
  }

  SECTION("--days 7 narrows to the recent tasks and keeps the scope") {
    auto d = dg::run(conn, request(1, 7), cat);
    REQUIRE(d.has_value());
    CHECK(d->window.source == dg::window_source::days);
    CHECK(d->window.days == 7);
    CHECK(d->window.from == "2026-05-25T00:00:00.000Z");
    CHECK(task_ids(*d) == std::vector<std::int64_t>{12, 13});
  }

  SECTION("a child plan scope excludes the anchor's own task") {
    auto d = dg::run(conn, request(2), cat);
    REQUIRE(d.has_value());
    CHECK(d->scope.plan_ids == std::vector<std::int64_t>{2, 3});
    CHECK(d->window.from == "2026-04-23T00:00:00.000Z");
    CHECK(task_ids(*d) == std::vector<std::int64_t>{12, 13});
  }

  SECTION("without a plan the window is --days, default 7, and every plan and plan-less task is in scope") {
    auto d = dg::run(conn, request(std::nullopt), cat);
    REQUIRE(d.has_value());
    CHECK(d->window.source == dg::window_source::default_days);
    CHECK(d->window.days == 7);
    CHECK_FALSE(d->scope.plan_id.has_value());
    CHECK(task_ids(*d) == std::vector<std::int64_t>{12, 13, 14, 15});
    auto wide = dg::run(conn, request(std::nullopt, 60), cat);
    REQUIRE(wide.has_value());
    CHECK(task_ids(*wide) == std::vector<std::int64_t>{11, 12, 13, 14, 15});
  }
}

TEST_CASE("the evaluation instant is injected and is the window's end", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_plan(conn, 1, std::nullopt, "2026-01-01T00:00:00.000Z");
  add_task(conn, 21, 1, 2, "2026-03-01T00:00:00.000Z");
  auto cat = catalog_of({task_check("task-seen")});

  auto early = dg::run(conn, request(1, std::nullopt, "2026-02-01T00:00:00.000Z"), cat);
  auto late  = dg::run(conn, request(1, std::nullopt, "2026-04-01T00:00:00.000Z"), cat);
  REQUIRE(early.has_value());
  REQUIRE(late.has_value());
  CHECK(early->evaluated_at == "2026-02-01T00:00:00.000Z");
  CHECK(early->window.to == "2026-02-01T00:00:00.000Z");
  CHECK(early->findings.empty());
  CHECK(task_ids(*late) == std::vector<std::int64_t>{21});

  auto whole_seconds = dg::run(conn, request(1, std::nullopt, "2026-04-01T00:00:00Z"), cat);
  REQUIRE(whole_seconds.has_value());
  CHECK(whole_seconds->evaluated_at == "2026-04-01T00:00:00.000Z");
}

TEST_CASE("bad input is a run_error and never an unavailable diagnosis", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_plan(conn, 1, std::nullopt, "2026-01-01T00:00:00.000Z");
  auto cat = catalog_of({task_check("task-seen")});

  auto unknown_plan = dg::run(conn, request(404), cat);
  REQUIRE_FALSE(unknown_plan.has_value());
  CHECK(unknown_plan.error().code == dg::run_error_code::unknown_plan);
  CHECK(unknown_plan.error().message.find("404") != std::string::npos);

  auto select_missing   = request(1);
  select_missing.checks = {"no-such-check"};
  auto unknown_check    = dg::run(conn, select_missing, cat);
  REQUIRE_FALSE(unknown_check.has_value());
  CHECK(unknown_check.error().code == dg::run_error_code::unknown_check);
  CHECK(unknown_check.error().message.find("no-such-check") != std::string::npos);

  auto zero_days = dg::run(conn, request(1, 0), cat);
  REQUIRE_FALSE(zero_days.has_value());
  CHECK(zero_days.error().code == dg::run_error_code::invalid_days);

  for (const char* bad : {"yesterday", "2026-06-01", "2026-06-01 00:00:00Z", "2026-13-01T00:00:00Z", "2026-06-01T00:00:00.5Z",
                          "2026-02-30T00:00:00Z", "2026-06-31T00:00:00Z", "2026-06-01T24:00:00Z", "2026-06-01T00:60:00Z"}) {
    INFO(bad);
    auto d = dg::run(conn, request(1, std::nullopt, bad), cat);
    REQUIRE_FALSE(d.has_value());
    CHECK(d.error().code == dg::run_error_code::invalid_instant);
  }
}

TEST_CASE("inputs of unselected or unbuilt checks are not_applicable and never degrade the outcome", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_plan(conn, 1, std::nullopt, "2026-05-01T00:00:00.000Z");
  add_task(conn, 31, 1, 3, "2026-05-30T00:00:00.000Z");

  int  capture_probes = 0;
  int  queue_probes   = 0;
  auto cat =
      catalog_of({task_check("claim-like", {}), task_check("cli-like", {"capture_log"}),
                  task_check("queue-like", {"queue_observation"}, false)},
                 {dg::input_def{.name  = "capture_log",
                                .probe = [&](const dg::check_context&) -> std::expected<dg::input_status, planar::db::db_error> {
                                  ++capture_probes;
                                  return dg::input_status{.state = im::coverage_state::disabled, .reason = "cli_log-off"};
                                }},
                  dg::input_def{.name  = "queue_observation",
                                .probe = [&](const dg::check_context&) -> std::expected<dg::input_status, planar::db::db_error> {
                                  ++queue_probes;
                                  return dg::input_status{.state = im::coverage_state::unavailable, .reason = "never-built"};
                                }}});

  auto find_row = [](const dg::diagnosis& d, std::string_view name) {
    auto it = std::ranges::find(d.coverage, name, &im::coverage_row::input);
    REQUIRE(it != d.coverage.end());
    return *it;
  };

  SECTION("selecting only the check that needs no input leaves the capture log not_applicable and the outcome ok") {
    auto req   = request(1);
    req.checks = {"claim-like"};
    auto d     = dg::run(conn, req, cat);
    REQUIRE(d.has_value());
    CHECK(d->result == dg::run_outcome::ok);
    CHECK(find_row(*d, "capture_log").state == im::coverage_state::not_applicable);
    CHECK(find_row(*d, "capture_log").reason == "not-selected");
    CHECK(find_row(*d, "queue_observation").state == im::coverage_state::not_applicable);
    CHECK(find_row(*d, "queue_observation").reason == "check-not-built");
    CHECK(capture_probes == 0);
    CHECK(queue_probes == 0);
    CHECK(d->findings.size() == 1);
  }

  SECTION("an unbuilt check never degrades the outcome, even when every built check runs") {
    auto req   = request(1);
    req.checks = {"claim-like"};
    auto d     = dg::run(conn, req, cat);
    REQUIRE(d.has_value());
    auto state_of = [&](std::string_view id) { return std::ranges::find(d->checks, id, &dg::check_summary::id)->state; };
    CHECK(state_of("claim-like") == dg::check_state::ran);
    CHECK(state_of("cli-like") == dg::check_state::not_selected);
    CHECK(state_of("queue-like") == dg::check_state::not_built);
  }

  SECTION("a selected check whose input is disabled reports nothing and the outcome is partial") {
    auto req   = request(1);
    req.checks = {"cli-like"};
    auto d     = dg::run(conn, req, cat);
    REQUIRE(d.has_value());
    CHECK(d->result == dg::run_outcome::partial);
    CHECK(find_row(*d, "capture_log").state == im::coverage_state::disabled);
    CHECK(find_row(*d, "capture_log").reason == "cli_log-off");
    CHECK(capture_probes == 1);
    CHECK(d->findings.empty());
    CHECK(std::ranges::find(d->checks, "cli-like", &dg::check_summary::id)->state == dg::check_state::input_unavailable);
    auto text = dg::render_text(*d);
    CHECK(text.find("input capture_log: disabled (cli_log-off)") != std::string::npos);
    CHECK(text.find("outcome partial") != std::string::npos);
  }

  SECTION("selecting every check probes the needed input once and is partial") {
    auto d = dg::run(conn, request(1), cat);
    REQUIRE(d.has_value());
    CHECK(d->result == dg::run_outcome::partial);
    CHECK(capture_probes == 1);
    CHECK(queue_probes == 0);
    CHECK(d->findings.size() == 1);
  }
}

TEST_CASE("an input that reads unavailable also makes the outcome partial", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_plan(conn, 1, std::nullopt, "2026-05-01T00:00:00.000Z");
  auto cat =
      catalog_of({task_check("needs-input", {"ledger"})},
                 {dg::input_def{.name  = "ledger",
                                .probe = [](const dg::check_context&) -> std::expected<dg::input_status, planar::db::db_error> {
                                  return dg::input_status{.state = im::coverage_state::unavailable, .reason = "unreadable"};
                                }}});
  auto d = dg::run(conn, request(1), cat);
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::partial);
}

TEST_CASE("findings come back in the fixed order in text and in JSON", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  add_plan(conn, 1, std::nullopt, "2026-05-01T00:00:00.000Z");
  add_task(conn, 9, 1, 2, "2026-05-30T00:00:00.000Z");
  add_task(conn, 10, 1, 2, "2026-05-30T00:00:00.000Z");
  add_task(conn, 5, 1, 3, "2026-05-30T00:00:00.000Z");
  add_task(conn, 4, 1, 1, "2026-05-30T00:00:00.000Z");

  // Two checks so the check id is part of the order; registration order is the reverse of the sort.
  auto cat = catalog_of({task_check("zz-check"), task_check("aa-check")});
  auto d   = dg::run(conn, request(1), cat);
  REQUIRE(d.has_value());

  std::vector<std::string> order;
  for (const auto& f : d->findings) {
    order.push_back(
        std::format("{} {} {}", im::diagnostic_severity_name(f.severity), f.check_id, im::entity_ref_text(f.primary)));
  }
  CHECK(order == std::vector<std::string>{"error aa-check task:5", "error zz-check task:5", "warning aa-check task:9",
                                          "warning aa-check task:10", "warning zz-check task:9", "warning zz-check task:10",
                                          "info aa-check task:4", "info zz-check task:4"});

  auto text = dg::render_text(*d);
  auto pos  = [&](std::string_view needle) { return text.find(needle); };
  CHECK(pos("error aa-check task:5 -> do the thing") != std::string::npos);
  CHECK(pos("error aa-check task:5") < pos("error zz-check task:5"));
  CHECK(pos("error zz-check task:5") < pos("warning aa-check task:9"));
  CHECK(pos("warning aa-check task:10") < pos("warning zz-check task:9"));
  CHECK(pos("warning zz-check task:10") < pos("info aa-check task:4"));

  auto parsed = planar::json_dom::parse_json(dg::render_json(*d));
  REQUIRE(parsed.has_value());
  CHECK(parsed->find("schema")->string == "planar.diagnose/1");
  CHECK(parsed->find("catalog_version")->integer == 1);
  CHECK(parsed->find("outcome")->string == "ok");
  CHECK(parsed->find("evaluated_at")->string == k_now);
  CHECK(parsed->find("scope")->find("window")->find("source")->string == "plan-lifetime");
  CHECK(parsed->find("scope")->find("plan_id")->integer == 1);
  REQUIRE(parsed->find("checks")->array.size() == 2);
  CHECK(parsed->find("would_resolve")->array.empty());
  const auto* findings = parsed->find("findings");
  REQUIRE(findings != nullptr);
  REQUIRE(findings->array.size() == d->findings.size());
  for (std::size_t i = 0; i < d->findings.size(); ++i) {
    const auto& f = findings->array[i];
    CHECK(f.find("check")->string == d->findings[i].check_id);
    CHECK(f.find("entity")->string == im::entity_ref_text(d->findings[i].primary));
    CHECK(f.find("fingerprint")->string == im::finding_fingerprint(d->findings[i]));
    CHECK(f.find("incident")->kind == planar::json_dom::json_kind::null_);
    CHECK(f.find("evidence_times")->array.size() == 1);
  }
}

TEST_CASE("a finding takes its check's id and recovery hint unless the check set one", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  dg::check_def   def;
  def.id       = "own-recovery";
  def.recovery = "default hint";
  def.evaluate = [](const dg::check_context&) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    im::finding a;
    a.check_id    = "spoofed-id";
    a.primary     = im::entity_ref{.kind = "task", .id = 1};
    a.evidence    = {a.primary};
    im::finding b = a;
    b.primary     = im::entity_ref{.kind = "task", .id = 2};
    b.evidence    = {b.primary};
    b.recovery    = "specific hint";
    return std::vector<im::finding>{a, b};
  };
  auto d = dg::run(conn, request(std::nullopt), catalog_of({def}));
  REQUIRE(d.has_value());
  REQUIRE(d->findings.size() == 2);
  CHECK(d->findings[0].check_id == "own-recovery");
  CHECK(d->findings[0].recovery == "default hint");
  CHECK(d->findings[1].recovery == "specific hint");
}

TEST_CASE("a lock held past 250 ms ends the run unavailable (busy) and the previous timeout is restored", "[diagnose][busy]") {
  scratch_db_path scratch;
  auto            writer = open_migrated(scratch);
  // WAL readers are never blocked; rollback-journal readers are, which is the case under test.
  exec(writer, "pragma journal_mode = delete");
  add_plan(writer, 1, std::nullopt, "2026-05-01T00:00:00.000Z");
  add_task(writer, 41, 1, 2, "2026-05-30T00:00:00.000Z");

  auto reader = planar::db::connection::open_existing(scratch.path_.string(), 4321);
  REQUIRE(reader.has_value());
  REQUIRE(busy_timeout_of(*reader) == 4321);
  auto cat = catalog_of({task_check("task-seen")});

  exec(writer, "begin exclusive");
  auto start = std::chrono::steady_clock::now();
  auto d     = dg::run(*reader, request(1, std::nullopt, "2026-06-01T00:00:00Z"), cat);
  auto took  = std::chrono::steady_clock::now() - start;
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::unavailable);
  REQUIRE(d->reason.has_value());
  CHECK(*d->reason == dg::unavailable_reason::busy);
  CHECK(d->evaluated_at == k_now);
  CHECK(d->findings.empty());
  // An impossible calendar date is bad input even while the database is busy.
  auto impossible = dg::run(*reader, request(1, std::nullopt, "2026-02-30T00:00:00Z"), cat);
  REQUIRE_FALSE(impossible.has_value());
  CHECK(impossible.error().code == dg::run_error_code::invalid_instant);
  // The bound is 250 ms, not the connection's 4.3 s.
  CHECK(took < std::chrono::milliseconds(3000));
  CHECK(busy_timeout_of(*reader) == 4321);
  CHECK(dg::render_text(*d) == "diagnose: unavailable (busy)\n");
  auto json = planar::json_dom::parse_json(dg::render_json(*d));
  REQUIRE(json.has_value());
  CHECK(json->find("outcome")->string == "unavailable");
  CHECK(json->find("reason")->string == "busy");

  exec(writer, "rollback");
  auto again = dg::run(*reader, request(1), cat);
  REQUIRE(again.has_value());
  CHECK(again->result == dg::run_outcome::ok);
  CHECK(task_ids(*again) == std::vector<std::int64_t>{41});
  CHECK(busy_timeout_of(*reader) == 4321);
}

TEST_CASE("a database without the planning tables is unavailable (schema-unsupported)", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto d = dg::run(*conn, request(std::nullopt), catalog_of({task_check("task-seen")}));
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::unavailable);
  REQUIRE(d->reason.has_value());
  CHECK(*d->reason == dg::unavailable_reason::schema_unsupported);
  CHECK(dg::render_text(*d) == "diagnose: unavailable (schema-unsupported)\n");
}

TEST_CASE("a failing check query ends the run unavailable (query-failed)", "[diagnose]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  dg::check_def   def;
  def.id       = "broken";
  def.evaluate = [](const dg::check_context& ctx) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    auto stmt = ctx.conn.prepare("select no_such_column from tasks");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    return std::vector<im::finding>{};
  };
  auto d = dg::run(conn, request(std::nullopt), catalog_of({def}));
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::unavailable);
  REQUIRE(d->reason.has_value());
  CHECK(*d->reason == dg::unavailable_reason::query_failed);
}

TEST_CASE("a run works on a read-only connection and leaves the database unchanged", "[diagnose]") {
  scratch_db_path scratch;
  {
    auto writer = open_migrated(scratch);
    build_forty_day_fixture(writer);
  }
  auto before = std::filesystem::file_size(scratch.path_);
  auto reader = planar::db::connection::open_read_only(scratch.path_.string());
  REQUIRE(reader.has_value());
  auto d = dg::run(*reader, request(1), catalog_of({task_check("task-seen")}));
  REQUIRE(d.has_value());
  CHECK(d->result == dg::run_outcome::ok);
  CHECK(task_ids(*d) == std::vector<std::int64_t>{11, 12, 13});
  CHECK_FALSE(reader->in_transaction());
  CHECK(std::filesystem::file_size(scratch.path_) == before);
}

TEST_CASE("the plan filter is a no-op without a plan and an id list with one", "[diagnose]") {
  CHECK(dg::plan_filter_sql(dg::plan_scope{}, "t.plan_id") == "1 = 1");
  CHECK(dg::plan_filter_sql(dg::plan_scope{.plan_id = 1, .plan_ids = {1, 2, 3}}, "t.plan_id") == "t.plan_id in (1, 2, 3)");
}
