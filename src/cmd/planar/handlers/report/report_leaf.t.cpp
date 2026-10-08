// @file report_leaf.t.cpp
// @brief In-process tests for `planar report` (plan 996, task 6352).
//
// ## `report` IS HERMETICALLY PARITY-TESTABLE
//
// A prior cycle's note claimed otherwise, having measured against the
// operator's REAL home directory. Under a pinned scratch `$HOME` (this
// file's `fixture` pins `HOME`, `PLANAR_DB`, `PLANAR_HOME`,
// `PLANAR_LOCAL_HOME`, exactly like `search_health_audit_leaves.t.cpp`),
// every vendor probes a directory that does not exist and reports
// `state=unavailable scanned=0` DETERMINISTICALLY — verified byte-for-byte
// against the built zig oracle under an identical pinned arena during this
// task's development (`report --json` and plain `report` both diffed
// clean). The three cases below assert the SAME three behaviors the task
// brief captured from that oracle run:
//
//     report --days 0   exit 2, "error: --days must be a positive integer (got 0)"
//     report --tail 0   exit 2, "error: --tail must be a positive integer (got 0)"
//     report --json     exit 0, {"version":"<build version
//     token>","schema_version":N,"health":"ok","window":30,"logging_enabled":false,...}
//
// ## THE POPULATED CASE EXERCISES THE REAL FILESYSTEM WALK
//
// The last case seeds a real `.claude/projects/*.jsonl` file under the
// fixture's scratch `$HOME` and asserts the resulting signal/coverage rows
// reach the rendered JSON — the actual `collect_vendor_path` directory-walk
// code path, not just the "nothing there" arm every other case exercises.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment every case dispatches
/// against. Same shape as `search_health_audit_leaves.t.cpp`'s `fixture`.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_report_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "fakehome", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief The second whitespace-separated token of `planar version`.
auto build_version_token(const fixture& fx) -> std::string {
  auto const         line = dispatch(fx, {"version"}).out;
  std::istringstream in(line);
  std::string        program;
  std::string        token;
  in >> program >> token;
  return token;
}

/// @brief Enable the capture log in the fixture's scratch config.
auto enable_cli_log(const fixture& fx) -> void {
  std::filesystem::create_directories(fx.root / "fakehome" / ".planar");
  std::ofstream cfg(fx.root / "fakehome" / ".planar" / "config.toml", std::ios::binary);
  cfg << "[introspection]\ncli_log = true\n";
}

/// @brief Run one SQL statement against the fixture's scratch database.
auto seed(const fixture& fx, std::string_view sql) -> void {
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar", "report"},
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto               conn = ctx.db().ensure_db();
  REQUIRE(conn.has_value());
  auto const applied = (*conn)->execute(sql);
  REQUIRE(applied.has_value());
}

} // namespace

TEST_CASE("report --days 0 refuses at exit 2, naming the offending value", "[cmd][report]") {
  auto const fx     = make_fixture("days0");
  auto const result = dispatch(fx, {"report", "--days", "0"});
  CHECK(result.code == 2);
  CHECK(result.err == "error: --days must be a positive integer (got 0)\n");
  CHECK(result.out.empty());
}

TEST_CASE("report --tail 0 refuses at exit 2, naming the offending value", "[cmd][report]") {
  auto const fx     = make_fixture("tail0");
  auto const result = dispatch(fx, {"report", "--tail", "0"});
  CHECK(result.code == 2);
  CHECK(result.err == "error: --tail must be a positive integer (got 0)\n");
  CHECK(result.out.empty());
}

TEST_CASE("report --days -5 refuses the same way a negative value does, not just zero", "[cmd][report]") {
  auto const fx     = make_fixture("daysneg");
  auto const result = dispatch(fx, {"report", "--days", "-5"});
  CHECK(result.code == 2);
  CHECK(result.err == "error: --days must be a positive integer (got -5)\n");
}

TEST_CASE("report --json on a fresh scratch arena reports the deterministic empty shape", "[cmd][report]") {
  // No `.claude`/`.codex`/`.copilot` under this fixture's `$HOME` at all —
  // every vendor probes a missing directory. The oracle's OWN behavior
  // under this exact shape (verified during development, not assumed):
  // every transcript vendor `state=unavailable scanned=0`, `cli_log`
  // `state=disabled` (logging is off by default — no config file at all
  // under this scratch `$HOME`).
  auto const fx     = make_fixture("json_empty");
  auto const result = dispatch(fx, {"report", "--json"});
  CHECK(result.code == 0);
  CHECK(result.err.empty());
  // `version` is the build's version token (the second token of the
  // `planar version` line), never the literal program name.
  auto const version = build_version_token(fx);
  CHECK(version != "planar");
  CHECK(result.out.starts_with(
      std::format(R"({{"version":"{}","schema_version":41,"health":"ok","window":30,"logging_enabled":false,)", version)));
  CHECK(result.out.find(R"("failure_tail":[])") != std::string::npos);
  CHECK(result.out.find(R"("invocations":[])") != std::string::npos);
  CHECK(result.out.find(R"("introspection_preview":{"signals":[],"coverage":[)") != std::string::npos);
  CHECK(result.out.find(R"({"vendor":"claude","state":"unavailable","scanned":0)") != std::string::npos);
  CHECK(result.out.find(R"({"vendor":"cli_log","state":"disabled","scanned":0)") != std::string::npos);
  CHECK(result.out.ends_with("}\n"));
}

TEST_CASE("report (no flags) renders the text form at exit 0", "[cmd][report]") {
  auto const fx     = make_fixture("text");
  auto const result = dispatch(fx, {"report"});
  CHECK(result.code == 0);
  CHECK(result.err.empty());
  CHECK(result.out.starts_with("=== planar diagnostic report ===\n"));
  CHECK(result.out.find("[introspection preview]\n") != std::string::npos);
  CHECK(result.out.find("  claude: state=unavailable scanned=0") != std::string::npos);
}

TEST_CASE("report refuses at exit 1 when config.toml exists but fails to parse", "[cmd][report][6352][iter2]") {
  // Pins F1: the oracle's `resolveConfig` (report.zig:98) fails OPEN only
  // when the file is simply missing (`file_content == null`, which
  // `cfg::resolve` treats as "use the embedded defaults" and succeeds).
  // A file that EXISTS but fails to parse makes `resolveConfig` itself
  // return `null`, and the caller reads that as `orelse
  // exit.die(ctx, error.InvalidConfig, "resolving report config", .{})` —
  // the verb REFUSES rather than silently reporting as if unconfigured.
  // Verified against the built zig oracle under an identical fixture:
  // exit 1, stderr "error: resolving report config\n", empty stdout.
  auto const fx = make_fixture("badconfig");
  std::filesystem::create_directories(fx.root / "fakehome" / ".planar");
  {
    std::ofstream file(fx.root / "fakehome" / ".planar" / "config.toml", std::ios::binary);
    file << "this is not valid toml [[[\n";
  }

  auto const result = dispatch(fx, {"report", "--json"});
  CHECK(result.code == 1);
  CHECK(result.err == "error: resolving report config\n");
  CHECK(result.out == planar::cmd::testsupport::json_error_envelope_line("report", "generic_failure"));
}

TEST_CASE("report's cli_log adapter reports a single row larger than the budget as byte_cap with one omitted row",
          "[cmd][report][6352][iter2][7364]") {
  // One `cli_invocations` row whose `verb_path` alone exceeds the handler's
  // 4 MiB default budget cannot be read at all. It is a budget outcome, not
  // an adapter failure: `cli_log` is observed with a `byte_cap` warning
  // carrying the omitted-row count, and no `cli_adapter_failed` is raised.
  // (A genuine query failure still maps to `cli_adapter_failed`; that
  // mapping is pinned by the adapter module's own test.)
  auto const fx = make_fixture("cli_single_row_over_budget");
  enable_cli_log(fx);
  std::string const huge_verb_path(4 * 1024 * 1024 + 4096, 'x');
  seed(fx,
       "insert into cli_invocations (verb_path, exit_code, recorded_at) values ('" + huge_verb_path + "', 0, datetime('now'))");

  auto const result = dispatch(fx, {"report", "--json"});
  CHECK(result.code == 0);
  CHECK(result.out.find(R"({"vendor":"cli_log","kind":"byte_cap","count":1})") != std::string::npos);
  CHECK(result.out.find(R"("kind":"cli_adapter_failed")") == std::string::npos);
  CHECK(result.out.find(R"({"vendor":"cli_log","state":"observed")") != std::string::npos);
}

TEST_CASE("report --json walks a real transcript directory under the fixture's scratch $HOME", "[cmd][report]") {
  auto const fx = make_fixture("populated");
  // `.claude/projects` is the built-in Claude path this handler resolves
  // relative to `$HOME` — populate it with one recognized malformed-ish
  // record and one genuinely unparseable line, so the coverage counts are
  // non-zero and this case is not vacuously satisfied by "nothing there"
  // the way every other case in this file legitimately is.
  auto const claude_dir = fx.root / "fakehome" / ".claude" / "projects";
  std::filesystem::create_directories(claude_dir);
  {
    std::ofstream file(claude_dir / "session1.jsonl", std::ios::binary);
    file << "not json at all\n";
  }

  auto const result = dispatch(fx, {"report", "--json"});
  CHECK(result.code == 0);
  CHECK(result.out.find(R"({"vendor":"claude","state":"observed","scanned":1,"malformed":1)") != std::string::npos);
  CHECK(result.out.find(R"({"vendor":"claude","kind":"malformed","count":1})") != std::string::npos);
}

TEST_CASE("report --json reads an oversize capture log newest first and warns byte_cap instead of failing",
          "[cmd][report][7364]") {
  // 20000 failed rows of roughly 230 bytes of JSONL each exceed the 4 MiB
  // budget but stay under the 50000-record cap. The newest ten rows carry a
  // recognizable verb path and so do the oldest ten; the middle is filler.
  auto const fx = make_fixture("cli_byte_cap");
  enable_cli_log(fx);
  seed(fx, R"(with recursive n(i) as (select 0 union all select i + 1 from n where i < 19999)
insert into cli_invocations (verb_path, exit_code, error_category, recorded_at)
select case when i < 10 then 'newest-marker'
            when i >= 19990 then 'oldest-marker'
            else 'filler-' || printf('%05d', i) || '-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' end,
       2, 'usage', datetime('now', '-' || i || ' seconds')
from n)");

  auto const result = dispatch(fx, {"report", "--json"});
  REQUIRE(result.code == 0);
  CHECK(result.out.find(R"("kind":"cli_adapter_failed")") == std::string::npos);
  CHECK(result.out.find(R"({"vendor":"cli_log","state":"observed")") != std::string::npos);
  CHECK(result.out.find(R"({"vendor":"cli_log","kind":"byte_cap","count":)") != std::string::npos);
  CHECK(result.out.find("planar newest-marker") != std::string::npos);
  CHECK(result.out.find("planar oldest-marker") == std::string::npos);
}

TEST_CASE("report --json carries failure_tail, logging_enabled and the real version", "[cmd][report][7368]") {
  auto const fx = make_fixture("json_fields");
  enable_cli_log(fx);
  seed(fx, "insert into cli_invocations (verb_path, exit_code, error_category, recorded_at) values"
           " ('task add', 2, 'usage', datetime('now', '-1 minutes')),"
           " ('plan show', 1, 'not_found', datetime('now', '-2 minutes'))");

  auto const result = dispatch(fx, {"report", "--json"});
  REQUIRE(result.code == 0);
  auto const version = build_version_token(fx);
  CHECK(version != "planar");
  CHECK(result.out.starts_with(std::format(R"({{"version":"{}",)", version)));
  CHECK(result.out.find(R"("logging_enabled":true)") != std::string::npos);
  CHECK(result.out.find(R"("failure_tail":[{"verb_path":"task add","error_category":"usage","exit_code":2,"recorded_at":")") !=
        std::string::npos);
  CHECK(result.out.find(R"({"verb_path":"plan show","error_category":"not_found","exit_code":1,"recorded_at":")") !=
        std::string::npos);

  // The text output prints the same rows.
  auto const text = dispatch(fx, {"report"});
  CHECK(text.out.find("  task add  cat=usage  exit=2  at=") != std::string::npos);
  CHECK(text.out.find("  plan show  cat=not_found  exit=1  at=") != std::string::npos);
}
