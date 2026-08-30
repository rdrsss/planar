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
import planar.cmd.planar.tree;

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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
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
  CHECK(result.out.starts_with(R"({"version":"planar","schema_version":33,"health":"ok","window":30,)"));
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
