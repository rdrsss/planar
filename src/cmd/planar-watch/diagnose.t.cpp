// @file diagnose.t.cpp
// @brief Tests for `planar-watch diagnose` (plan 1132, task 7373): the exit mapping
// in-process over a caller-supplied catalog, and a realistic workflow through the built
// binaries.
//
// What is pinned:
//
//   * Exit 0 whenever the run completed: with findings, and with a partial outcome.
//   * Exit 1 when the run did not complete (`unavailable`), with the result still printed.
//   * Exit 2 for bad input -- an unknown check or plan, `--days` below 1 and an unknown flag
//     (`--run`, `--session`) -- with nothing on standard output.
//   * Through the real binaries: a plan built with `planar` diagnoses clean, outcome ok,
//     with the inputs no built check needs reported not_applicable.
//   * The verb leaves the database file unchanged.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.engine.diagnose;
import planar.incident_model;
import planar.json_dom;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.diagnose;
import planar.cmd.planar_watch.main;

#include "parity_harness.hpp"

namespace {

namespace dg = planar::engine::diagnose;
namespace im = planar::incident_model;

struct scratch {
  std::filesystem::path dir;
  std::filesystem::path db;

  scratch() {
    dir = std::filesystem::temp_directory_path() /
          std::format("planar_watch_diagnose_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    db          = dir / "planar.db";
    auto seeded = planar::db::connection::open(db.string());
    REQUIRE(seeded.has_value());
    REQUIRE(planar::db::apply_all(*seeded).has_value());
    REQUIRE(seeded
                ->execute("insert into plans (id, scope_kind, title, slug, created_at) values (1, 'global', 'p', 'p', "
                          "'2020-01-01T00:00:00.000Z')")
                .has_value());
    REQUIRE(seeded
                ->execute("insert into tasks (id, scope_kind, plan_id, title, created_at) values (5, 'global', 1, 't', "
                          "'2020-01-02T00:00:00.000Z')")
                .has_value());
  }
  scratch(const scratch&)            = delete;
  scratch& operator=(const scratch&) = delete;
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  [[nodiscard]] auto bytes() const -> std::string {
    std::ifstream in(db, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

struct outcome {
  int         code = -1;
  std::string out;
  std::string err;
};

/// A catalog with a check that reports one error for each task, and optionally an input that is disabled
/// and a check that fails.
auto test_catalog(bool disabled_input, bool failing, bool unavailable_input = false) -> dg::catalog {
  dg::catalog   cat;
  dg::check_def def;
  def.id       = "task-seen";
  def.kind     = im::check_kind::event;
  def.severity = im::diagnostic_severity::error;
  def.category = "test";
  def.recovery = "look at it";
  if (unavailable_input) {
    def.inputs = {"capture_log"};
    cat.inputs.push_back(dg::input_def{
        .name = "capture_log", .probe = [](const dg::check_context&) -> std::expected<dg::input_status, planar::db::db_error> {
          return dg::input_status{.state = im::coverage_state::unavailable, .reason = "cli_log-config-unknown"};
        }});
  } else if (disabled_input) {
    def.inputs = {"capture_log"};
    cat.inputs.push_back(dg::input_def{
        .name = "capture_log", .probe = [](const dg::check_context&) -> std::expected<dg::input_status, planar::db::db_error> {
          return dg::input_status{.state = im::coverage_state::disabled, .reason = "cli_log-off"};
        }});
  }
  def.evaluate = [failing](const dg::check_context& ctx) -> std::expected<std::vector<im::finding>, planar::db::db_error> {
    auto stmt = ctx.conn.prepare(failing ? "select no_such_column from tasks" : "select id, created_at from tasks order by id");
    if (!stmt) {
      return std::unexpected(stmt.error());
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
      f.severity       = im::diagnostic_severity::error;
      f.primary        = im::entity_ref{.kind = "task", .id = stmt->column_int64(0)};
      f.evidence       = {f.primary};
      f.evidence_times = {stmt->column_text(1)};
      out.push_back(std::move(f));
    }
    return out;
  };
  cat.checks.push_back(std::move(def));
  return cat;
}

/// Runs `planar-watch diagnose <extra...>` in-process with `cat` behind the verb.
auto run_in_process(const scratch& fx, std::vector<std::string> extra, const dg::catalog& cat) -> outcome {
  std::vector<std::string> argv{"planar-watch", "diagnose"};
  argv.insert(argv.end(), extra.begin(), extra.end());
  std::ostringstream          out;
  std::ostringstream          err;
  planar::cmd::watch::context ctx{argv,   planar::cmd::watch::map_env({{"PLANAR_DB", fx.db.string()}}),
                                  fx.dir, std::make_shared<planar::cmd::watch::database>(fx.db, err),
                                  out,    err};
  // Only the verb under test needs a handler: it is a leaf, so the table need hold nothing else.
  planar::cmd::watch::handler_table table;
  table.emplace("diagnose", [&cat](planar::cmd::watch::context& c, const planar::cliapp::parsed_args& a) {
    return planar::cmd::watch::handlers::diagnose_with_catalog(c, a, cat);
  });
  auto const fresh = planar::cmd::watch::root_app();
  int const  code  = planar::cmd::watch::run(ctx, *fresh, table);
  return outcome{.code = code, .out = out.str(), .err = err.str()};
}

} // namespace

TEST_CASE("diagnose exits 0 with findings present and prints them in text and JSON", "[cmd][watch][diagnose]") {
  scratch fx;
  auto    text = run_in_process(fx, {"--plan", "1"}, test_catalog(false, false));
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.out.contains("outcome ok"));
  CHECK(text.out.contains("error task-seen task:5 -> look at it"));

  auto json = run_in_process(fx, {"--plan", "1", "--json"}, test_catalog(false, false));
  CHECK(json.code == 0);
  auto parsed = planar::json_dom::parse_json(json.out);
  REQUIRE(parsed.has_value());
  CHECK(parsed->find("schema")->string == "planar.diagnose/1");
  CHECK(parsed->find("findings")->array.size() == 1);
  CHECK(parsed->find("scope")->find("window")->find("source")->string == "plan-lifetime");
}

TEST_CASE("diagnose exits 0 for a partial outcome and says which input was not read", "[cmd][watch][diagnose]") {
  scratch fx;
  auto    got = run_in_process(fx, {"--plan", "1"}, test_catalog(false, false, true));
  CHECK(got.code == 0);
  CHECK(got.out.contains("outcome partial"));
  CHECK(got.out.contains("input capture_log: unavailable (cli_log-config-unknown)"));
  CHECK_FALSE(got.out.contains("task-seen"));
}

TEST_CASE("a disabled input is named but does not make the outcome partial", "[cmd][watch][diagnose]") {
  scratch fx;
  auto    got = run_in_process(fx, {"--plan", "1"}, test_catalog(true, false));
  CHECK(got.code == 0);
  CHECK(got.out.contains("outcome ok"));
  CHECK(got.out.contains("input capture_log: disabled (cli_log-off)"));
  CHECK_FALSE(got.out.contains("task-seen"));
}

TEST_CASE("diagnose exits 1 and still prints the result when the run did not complete", "[cmd][watch][diagnose]") {
  scratch fx;
  auto    text = run_in_process(fx, {}, test_catalog(false, true));
  CHECK(text.code == 1);
  CHECK(text.out == "diagnose: unavailable (query-failed)\n");
  CHECK(text.err.empty());

  auto json = run_in_process(fx, {"--json"}, test_catalog(false, true));
  CHECK(json.code == 1);
  auto parsed = planar::json_dom::parse_json(json.out);
  REQUIRE(parsed.has_value());
  CHECK(parsed->find("outcome")->string == "unavailable");
  CHECK(parsed->find("reason")->string == "query-failed");
}

TEST_CASE("diagnose exits 2 for bad input and prints nothing on standard output", "[cmd][watch][diagnose]") {
  scratch    fx;
  auto const cat = test_catalog(false, false);
  struct bad {
    std::vector<std::string> args;
    std::string_view         names;
  };
  for (auto const& c : std::vector<bad>{{{"--check", "no-such-check"}, "no-such-check"},
                                        {{"--check", "task-seen", "--check", "nope"}, "nope"},
                                        {{"--plan", "999"}, "999"},
                                        {{"--days", "0"}, "--days"},
                                        {{"--days", "-3"}, "--days"},
                                        {{"--run", "1"}, "--run"},
                                        {{"--session", "1"}, "--session"},
                                        {{"--days", "x"}, "--days"}}) {
    auto got = run_in_process(fx, c.args, cat);
    INFO("args: " << c.args.back());
    CHECK(got.code == 2);
    CHECK(got.out.empty());
    CHECK(got.err.contains(c.names));
  }
}

TEST_CASE("diagnose selects checks with a repeatable --check", "[cmd][watch][diagnose]") {
  scratch fx;
  auto    got = run_in_process(fx, {"--check", "task-seen", "--json"}, test_catalog(false, false));
  CHECK(got.code == 0);
  auto parsed = planar::json_dom::parse_json(got.out);
  REQUIRE(parsed.has_value());
  CHECK(parsed->find("findings")->array.size() == 1);
}

TEST_CASE("diagnose leaves the database file unchanged", "[cmd][watch][diagnose]") {
  scratch    fx;
  auto const before = fx.bytes();
  auto const time   = std::filesystem::last_write_time(fx.db);
  for (auto const& extra : std::vector<std::vector<std::string>>{{}, {"--json"}, {"--plan", "1", "--days", "9999"}}) {
    CHECK(run_in_process(fx, extra, test_catalog(false, false)).code == 0);
  }
  CHECK(fx.bytes() == before);
  CHECK((std::filesystem::last_write_time(fx.db) == time));
}

TEST_CASE("a plan built with the operator binary diagnoses clean through planar-watch", "[cmd][watch][diagnose][workflow]") {
  using planar::cmd::parity::make_arena;
  using planar::cmd::parity::run_pinned;
  auto const arena = make_arena("watch_diagnose");
  auto const root  = arena.cpp_root;
  auto const op    = std::filesystem::path{PLANAR_OPERATOR_CPP_BIN};
  auto const watch = std::filesystem::path{PLANAR_CPP_BIN};

  auto step = [&](std::vector<std::string> args, std::string_view tag) {
    auto got = run_pinned(op, args, root, tag);
    INFO("seed " << tag << ": " << got.err);
    REQUIRE(got.code == 0);
  };
  step({"init"}, "s0");
  step({"assoc", "create", "project:proj", "--kind", "project"}, "s1");
  step({"assoc", "add", "project:proj", (root / "proj").string()}, "s2");
  step({"plan", "create", "Demo plan"}, "s3");
  step({"task", "add", "First task", "--plan", "1"}, "s4");
  step({"plan", "update", "1", "--status", "active"}, "s5");

  auto json = run_pinned(watch, std::vector<std::string>{"diagnose", "--plan", "1", "--json"}, root, "d1");
  INFO("stderr: " << json.err);
  REQUIRE(json.code == 0);
  auto parsed = planar::json_dom::parse_json(json.out);
  REQUIRE(parsed.has_value());
  CHECK(parsed->find("schema")->string == "planar.diagnose/1");
  CHECK(parsed->find("outcome")->string == "ok");
  CHECK(parsed->find("findings")->array.empty());
  CHECK(parsed->find("scope")->find("plan_id")->integer == 1);
  CHECK(parsed->find("scope")->find("window")->find("source")->string == "plan-lifetime");
  bool saw_run_identity = false;
  for (auto const& row : parsed->find("coverage")->array) {
    // The default config leaves the capture log off, so its input reads `disabled`, which does not degrade the
    // outcome; the other inputs are for unbuilt checks.
    CHECK(row.find("state")->string == (row.find("input")->string == "cli_log" ? "disabled" : "not_applicable"));
    saw_run_identity = saw_run_identity || row.find("input")->string == "run_identity";
  }
  CHECK(saw_run_identity);

  auto text = run_pinned(watch, std::vector<std::string>{"diagnose", "--plan", "1"}, root, "d2");
  CHECK(text.code == 0);
  CHECK(text.out.contains("plan 1 (1 plan(s))"));
  CHECK(text.out.contains("outcome ok"));

  // Bad input through the real binary: exit 2 and an empty standard output.
  for (auto const& args : std::vector<std::vector<std::string>>{{"diagnose", "--check", "no-such-check"},
                                                                {"diagnose", "--plan", "999"},
                                                                {"diagnose", "--days", "0"},
                                                                {"diagnose", "--run", "1"}}) {
    auto got = run_pinned(watch, args, root, "bad");
    INFO("args: " << args.back() << " stderr: " << got.err);
    CHECK(got.code == 2);
    CHECK(got.out.empty());
  }
}
