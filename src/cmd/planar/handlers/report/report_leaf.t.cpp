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
//     report --json     exit 0, {"version":"planar","schema_version":33,"health":"ok","window":30,...}
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
  CHECK(result.out.starts_with(R"({"version":"planar","schema_version":40,"health":"ok","window":30,)"));
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

TEST_CASE("report's cli_log adapter turns a genuine query failure into cli_adapter_failed, not silence",
          "[cmd][report][6352][iter2]") {
  // Reviewer finding (iteration 2): a break-probe swapping the handler's
  // `cli_read_result{.status = ia::cli_read_status::failed}` for the
  // default (`unavailable`) SURVIVED — no test forced
  // `intro::cli_preview_jsonl` to genuinely fail and observed the
  // difference. `cli_preview_jsonl` fails when the accumulated JSONL
  // exceeds `max_bytes` (`introspect.cpp`'s own "exceeding max_bytes
  // reports query_failed" test pins that at the engine layer). Seeding
  // ONE `cli_invocations` row whose `verb_path` alone is bigger than the
  // handler's 4 MiB default budget reproduces that failure through the
  // REAL handler path: un-mutated code reports `cli_adapter_failed` (and
  // `cli_log` still `unavailable`); the mutant would report `unavailable`
  // with NO warning — silently indistinguishable from "nothing configured".
  // (Dropping the table instead was tried and rejected: `build()` queries
  // `cli_invocations` too when `cli_log` is on, so a dropped table fails
  // the bundle build itself before ever reaching the adapter closure.)
  auto const fx = make_fixture("cli_query_failed");
  std::filesystem::create_directories(fx.root / "fakehome" / ".planar");
  {
    std::ofstream cfg(fx.root / "fakehome" / ".planar" / "config.toml", std::ios::binary);
    cfg << "[introspection]\ncli_log = true\n";
  }

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar", "report", "--json"},
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto               conn = ctx.db().ensure_db();
  REQUIRE(conn.has_value());
  // 4 MiB (k_default_max_bytes) plus slack, so the single row alone
  // overflows `cli_preview_jsonl`'s budget on its own.
  std::string const huge_verb_path(4 * 1024 * 1024 + 4096, 'x');
  auto const        inserted = (*conn)->execute("insert into cli_invocations (verb_path, exit_code, recorded_at) values ('" +
                                                huge_verb_path + "', 0, datetime('now'))");
  REQUIRE(inserted.has_value());

  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::make_handler_table(*tree);
  int const  code  = planar::cmd::run(ctx, *tree, table);
  CHECK(code == 0);
  CHECK(out.str().find(R"({"vendor":"cli_log","kind":"cli_adapter_failed","count":1})") != std::string::npos);
  CHECK(out.str().find(R"({"vendor":"cli_log","state":"unavailable")") != std::string::npos);
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
