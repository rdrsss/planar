// @file checks_cli.t.cpp
// @brief Tests for the CLI-derived checks of `planar.engine.diagnose` (plan 1132, task 7379).
//
// These cases drive the shipped catalog over a scratch database with `cli_invocations` rows written
// directly, so the evaluation instant and every boundary are exact. The same check runs through
// the real `planar` verbs and `planar-watch` in `src/cmd/planar-watch/diagnose_checks.t.cpp`.
//
// What is pinned:
//
//   * `apply-without-preview` (event, warning): a `spec ingest` invocation whose flags include
//     `--apply`, with no earlier successful-or-not `spec ingest` invocation without `--apply` in the
//     window. `--apply-removals` is a different flag; another verb with `--apply` is not an ingest.
//   * The capture log is the check's only input (`cli_log`): off reads `disabled`, the check does not
//     run (no finding even over rows an earlier logging period left) and the outcome stays `ok`, since
//     the operator turned the input off; an unknown setting reads `unavailable` and the outcome
//     `partial`; on reads `observed`.
//   * The positional-shape limit: `args_shape` keeps only the arity of a positional, so a preview of
//     any plan satisfies an apply of any other.
//   * Evidence is the invocation's row reference and its `recorded_at`, never its arguments.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.diagnose;
import planar.incident_model;

namespace {

namespace dg = planar::engine::diagnose;
namespace im = planar::incident_model;

constexpr std::string_view k_now = "2026-06-01T12:00:00.000Z";

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_diagnose_cli_{}_{}.db",
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

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

struct fixture {
  scratch_db_path        scratch;
  planar::db::connection conn;

  fixture() : conn(open(scratch)) {
    exec(conn, "insert into plans (id, scope_kind, title, slug, created_at) values (1, 'global', 'p1', 'p1', "
               "'2026-01-01T00:00:00.000Z')");
  }

  static auto open(const scratch_db_path& scratch) -> planar::db::connection {
    auto conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    return std::move(*conn);
  }

  /// One logged invocation. A zero exit has no error category; any other exit is a `usage` failure.
  auto invocation(int id, std::string_view verb_path, std::string_view args_shape, std::string_view recorded_at,
                  int exit_code = 0) -> void {
    exec(conn, std::format("insert into cli_invocations (id, verb_path, args_shape, exit_code, error_category, recorded_at) "
                           "values ({}, '{}', '{}', {}, {}, '{}')",
                           id, verb_path, args_shape, exit_code, exit_code == 0 ? std::string{"null"} : std::string{"'usage'"},
                           recorded_at));
  }

  auto run(std::vector<std::string> checks, std::optional<bool> cli_log = true, std::string_view at = k_now,
           std::optional<int> plan = std::nullopt, std::optional<int> days = std::nullopt) -> dg::diagnosis {
    auto result = dg::run(conn, dg::run_request{.plan_id         = plan,
                                                .days            = days,
                                                .checks          = std::move(checks),
                                                .evaluated_at    = std::string{at},
                                                .cli_log_enabled = cli_log});
    REQUIRE(result.has_value());
    return std::move(*result);
  }
};

auto ids_of(const dg::diagnosis& d) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& f : d.findings) {
    out.push_back(std::format("{} {}", f.check_id, im::entity_ref_text(f.primary)));
  }
  return out;
}

auto coverage_of(const dg::diagnosis& d, std::string_view input) -> im::coverage_row {
  auto it = std::ranges::find(d.coverage, input, &im::coverage_row::input);
  REQUIRE(it != d.coverage.end());
  return *it;
}

const std::vector<std::string> k_apply{"apply-without-preview"};

} // namespace

TEST_CASE("apply-without-preview is catalogued with the spec's kind, severity, category and input", "[engine][diagnose][cli]") {
  auto cat = dg::builtin_catalog();
  auto it  = std::ranges::find(cat.checks, "apply-without-preview", &dg::check_def::id);
  REQUIRE(it != cat.checks.end());
  CHECK(it->built);
  CHECK(it->kind == im::check_kind::event);
  CHECK(it->severity == im::diagnostic_severity::warning);
  CHECK(it->category == "cli_apply_without_preview");
  CHECK(it->inputs == std::vector<std::string>{"cli_log"});
  CHECK(it->recovery.contains("without --apply"));
  auto input = std::ranges::find(cat.inputs, "cli_log", &dg::input_def::name);
  REQUIRE(input != cat.inputs.end());
  CHECK(input->built);
}

TEST_CASE("apply-without-preview reports an ingest apply with no preview before it", "[engine][diagnose][cli]") {
  fixture fx;
  fx.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");

  auto d = fx.run(k_apply);
  CHECK(d.result == dg::run_outcome::ok);
  REQUIRE(d.findings.size() == 1);
  const auto& f = d.findings[0];
  CHECK(f.check_id == "apply-without-preview");
  CHECK(f.severity == im::diagnostic_severity::warning);
  CHECK(im::entity_ref_text(f.primary) == "cli_invocation:1");
  // Entity references and a timestamp only: nothing of the arguments.
  CHECK(f.evidence == std::vector<im::entity_ref>{im::entity_ref{.kind = "cli_invocation", .id = 1}});
  CHECK(f.evidence_times == std::vector<std::string>{"2026-06-01T09:00:00.000Z"});
  CHECK(f.recovery.contains("without --apply"));
}

TEST_CASE("a preview before the apply satisfies it, a preview after does not", "[engine][diagnose][cli]") {
  fixture before;
  before.invocation(1, "spec ingest", "<pos:1>", "2026-06-01T09:00:00.000Z");
  before.invocation(2, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z");
  CHECK(before.run(k_apply).findings.empty());

  fixture after;
  after.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");
  after.invocation(2, "spec ingest", "<pos:1>", "2026-06-01T09:05:00.000Z");
  CHECK(ids_of(after.run(k_apply)) == std::vector<std::string>{"apply-without-preview cli_invocation:1"});

  // One preview precedes every later apply; the rule asks for a preceding preview, not one each.
  fixture twice;
  twice.invocation(1, "spec ingest", "<pos:1> --json", "2026-06-01T09:00:00.000Z");
  twice.invocation(2, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z");
  twice.invocation(3, "spec ingest", "<pos:1> --apply", "2026-06-01T09:10:00.000Z");
  CHECK(twice.run(k_apply).findings.empty());
}

TEST_CASE("invocations stamped with the same instant are ordered by their row ids", "[engine][diagnose][cli]") {
  fixture preview_first;
  preview_first.invocation(1, "spec ingest", "<pos:1>", "2026-06-01T09:00:00.000Z");
  preview_first.invocation(2, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");
  CHECK(preview_first.run(k_apply).findings.empty());

  fixture apply_first;
  apply_first.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");
  apply_first.invocation(2, "spec ingest", "<pos:1>", "2026-06-01T09:00:00.000Z");
  CHECK(ids_of(apply_first.run(k_apply)) == std::vector<std::string>{"apply-without-preview cli_invocation:1"});
}

TEST_CASE("a preview of any plan satisfies an apply of another: the positional-shape limit", "[engine][diagnose][cli]") {
  // args_shape keeps only `<pos:1>`, so these two rows are indistinguishable from a preview and an
  // apply of the same plan. docs/cli-reference.md states the limit.
  fixture fx;
  fx.invocation(1, "spec ingest", "<pos:1>", "2026-06-01T09:00:00.000Z");
  fx.invocation(2, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z");
  CHECK(fx.run(k_apply).findings.empty());
}

TEST_CASE("only an exact --apply flag on spec ingest is an apply", "[engine][diagnose][cli]") {
  fixture fx;
  // A different flag that starts the same way, an unknown flag, and an apply of another verb.
  fx.invocation(1, "spec ingest", "<pos:1> --apply-removals", "2026-06-01T09:00:00.000Z");
  fx.invocation(2, "spec ingest", "<pos:1> --<unknown>", "2026-06-01T09:01:00.000Z");
  fx.invocation(3, "spec draft", "<pos:1> --apply", "2026-06-01T09:02:00.000Z");
  fx.invocation(4, "task update", "<pos:1> --apply", "2026-06-01T09:03:00.000Z");
  fx.invocation(5, "spec", "--apply", "2026-06-01T09:04:00.000Z");
  CHECK(fx.run(k_apply).findings.empty());

  // The flag in any position among the others is an apply; the earlier rows above are not previews of
  // another verb's apply, but they are `spec ingest` rows without the flag and so previews.
  fixture position;
  position.invocation(1, "spec ingest", "--apply", "2026-06-01T09:00:00.000Z");
  position.invocation(2, "spec ingest", "<pos:1> --json --apply", "2026-06-01T09:01:00.000Z");
  position.invocation(3, "spec ingest", "<pos:1> --apply --json", "2026-06-01T09:02:00.000Z");
  CHECK(ids_of(position.run(k_apply)) == std::vector<std::string>{"apply-without-preview cli_invocation:1",
                                                                  "apply-without-preview cli_invocation:2",
                                                                  "apply-without-preview cli_invocation:3"});
}

TEST_CASE("a preview of another verb is not a preview of an ingest", "[engine][diagnose][cli]") {
  fixture fx;
  fx.invocation(1, "spec draft", "<pos:1>", "2026-06-01T09:00:00.000Z");
  fx.invocation(2, "task show", "<pos:1>", "2026-06-01T09:01:00.000Z");
  fx.invocation(3, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z");
  CHECK(ids_of(fx.run(k_apply)) == std::vector<std::string>{"apply-without-preview cli_invocation:3"});
}

TEST_CASE("a failed preview or a failed apply still counts", "[engine][diagnose][cli]") {
  // A preview that exited 2 ran and printed its report; a failed apply is still an attempt to apply.
  fixture fx;
  fx.invocation(1, "spec ingest", "<pos:1>", "2026-06-01T09:00:00.000Z", 2);
  fx.invocation(2, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z", 2);
  CHECK(fx.run(k_apply).findings.empty());

  fixture lone;
  lone.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z", 2);
  CHECK(ids_of(lone.run(k_apply)) == std::vector<std::string>{"apply-without-preview cli_invocation:1"});
}

TEST_CASE("apply-without-preview looks only inside the window", "[engine][diagnose][cli]") {
  // The default window is seven days: 2026-05-25T12:00 through the evaluation instant.
  fixture stale_preview;
  stale_preview.invocation(1, "spec ingest", "<pos:1>", "2026-05-01T09:00:00.000Z");
  stale_preview.invocation(2, "spec ingest", "<pos:1> --apply", "2026-06-01T09:05:00.000Z");
  CHECK(ids_of(stale_preview.run(k_apply)) == std::vector<std::string>{"apply-without-preview cli_invocation:2"});
  // A wider window sees the preview.
  CHECK(stale_preview.run(k_apply, true, k_now, std::nullopt, 60).findings.empty());

  fixture old_apply;
  old_apply.invocation(1, "spec ingest", "<pos:1> --apply", "2026-05-01T09:05:00.000Z");
  CHECK(old_apply.run(k_apply).findings.empty());
  CHECK(old_apply.run(k_apply, true, k_now, std::nullopt, 60).findings.size() == 1);
}

TEST_CASE("the invocation log carries no plan, so a plan scope changes the window and nothing else", "[engine][diagnose][cli]") {
  fixture fx;
  fx.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");
  // Plan 1 was created on 2026-01-01, so its lifetime window reaches the invocation.
  CHECK(ids_of(fx.run(k_apply, true, k_now, 1)) == std::vector<std::string>{"apply-without-preview cli_invocation:1"});
}

TEST_CASE("apply-without-preview times its evidence from the row, not from the evaluation instant", "[engine][diagnose][cli]") {
  fixture fx;
  fx.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");
  auto first = fx.run(k_apply);
  auto later = fx.run(k_apply, true, "2026-06-02T12:00:00.000Z");
  REQUIRE(first.findings.size() == 1);
  REQUIRE(later.findings.size() == 1);
  CHECK(im::finding_fingerprint(first.findings[0]) == im::finding_fingerprint(later.findings[0]));
  CHECK(im::finding_digest(first.findings[0]) == im::finding_digest(later.findings[0]));
}

TEST_CASE("with the capture log off the input is disabled, the outcome stays ok and nothing is reported",
          "[engine][diagnose][cli]") {
  // An earlier logging period left an apply with no preview; a log that is off now must not be read as evidence.
  fixture fx;
  fx.invocation(1, "spec ingest", "<pos:1> --apply", "2026-06-01T09:00:00.000Z");

  auto d = fx.run(k_apply, false);
  CHECK(d.result == dg::run_outcome::ok);
  CHECK(d.findings.empty());
  auto row = coverage_of(d, "cli_log");
  CHECK(row.state == im::coverage_state::disabled);
  CHECK(row.reason == "cli_log-off");
  auto summary = std::ranges::find(d.checks, "apply-without-preview", &dg::check_summary::id);
  REQUIRE(summary != d.checks.end());
  CHECK(summary->state == dg::check_state::input_unavailable);

  // A run that selects every check reads the same.
  CHECK(fx.run({}, false).result == dg::run_outcome::ok);
}

TEST_CASE("an unknown capture-log setting reads unavailable, never clean", "[engine][diagnose][cli]") {
  fixture fx;
  auto    d = fx.run(k_apply, std::nullopt);
  CHECK(d.result == dg::run_outcome::partial);
  CHECK(d.findings.empty());
  auto row = coverage_of(d, "cli_log");
  CHECK(row.state == im::coverage_state::unavailable);
  CHECK(row.reason == "cli_log-config-unknown");
}

TEST_CASE("with the capture log on the input is observed", "[engine][diagnose][cli]") {
  fixture fx;
  auto    d = fx.run(k_apply, true);
  CHECK(d.result == dg::run_outcome::ok);
  auto row = coverage_of(d, "cli_log");
  CHECK(row.state == im::coverage_state::observed);
  CHECK(row.reason.empty());
}

TEST_CASE("the capture-log input is not applicable when no selected check needs it", "[engine][diagnose][cli]") {
  // Deselected, the check cannot degrade the outcome whatever the setting.
  fixture fx;
  auto    d = fx.run({"task-doing-unclaimed"}, false);
  CHECK(d.result == dg::run_outcome::ok);
  auto row = coverage_of(d, "cli_log");
  CHECK(row.state == im::coverage_state::not_applicable);
  CHECK(row.reason == "not-selected");
}
