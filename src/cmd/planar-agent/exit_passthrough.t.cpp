// @file exit_passthrough.t.cpp
// @brief A handler's pass-through exit status reaches the process (plan
// 1080, task 7007, test-spec scenario "Happy path — a handler's exit status
// reaches the process").
//
// No verb in this build returns a pass-through status yet (`queue run` is
// task 7008), so these cases register a TEST handler over an existing leaf
// in a copy of the real table and drive the real `run`. The leaf's own
// parse rules still apply; only the handler behind it is replaced.
//
// The last case shells the built binary and requires its exit code to equal
// what the in-process `run` returns for the same argv: that is the half the
// in-process cases cannot see, `main` forwarding `run`'s value unchanged.
//
// HOME / DB SAFETY. In-process cases build their own environment map over
// a unique scratch root; the black-box case goes through `run_pinned`.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.main;

#include "parity_harness.hpp"

namespace {

using planar::cmd::agent::context;
using planar::cmd::agent::domain_error_kind;
using planar::cmd::agent::error_from_body;
using planar::cmd::agent::exit_status;
using planar::cmd::agent::handler_outcome;
using planar::cmd::agent::handler_result;
using planar::cmd::agent::handler_table;

/// @brief One dispatch's observable result.
struct invocation {
  int         code = -1; ///< The exit code `run` returned.
  std::string out;       ///< Everything written to stdout.
  std::string err;       ///< Everything written to stderr.
};

/// @brief A unique scratch root with the environment a case dispatches under.
struct scratch {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
};

/// @brief Build a scratch root.
/// @param tag A short discriminator so a failure names its own case.
/// @return The scratch root.
auto make_scratch(std::string_view tag) -> scratch {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_cmd_agent_xp_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  return scratch{
      .root = root,
      .vars = {{"PLANAR_HOME", (root / "home").string()},
               {"HOME", (root / "fakehome").string()},
               {"PWD", (root / "proj").string()},
               {"PLANAR_DB", (root / "planar.db").string()}},
  };
}

/// @brief Run `args` through the real tree with the real table, after
/// `patch` has had the chance to replace entries in it.
/// @param tag Scratch discriminator.
/// @param args The argv tail.
/// @param patch Edits the table before dispatch.
/// @return The captured invocation.
auto dispatch_with(std::string_view tag, std::vector<std::string> args, const std::function<void(handler_table&)>& patch)
    -> invocation {
  auto const               sc = make_scratch(tag);
  std::vector<std::string> argv{"planar-agent"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::agent::map_env(sc.vars),
                         sc.root / "proj",
                         std::make_shared<planar::cmd::agent::database>(sc.root / "planar.db", err),
                         out,
                         err};
  auto const         tree  = planar::cmd::agent::root_app();
  auto               table = planar::cmd::agent::handlers(*tree);
  patch(table);
  int const       code = planar::cmd::agent::run(ctx, *tree, table);
  std::error_code ec;
  std::filesystem::remove_all(sc.root, ec);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run `version` with its handler replaced by one returning `status`.
/// @param status The status the test handler passes through.
/// @return The captured invocation.
auto version_returning(int status) -> invocation {
  return dispatch_with("status", {"version"}, [status](handler_table& table) {
    table.insert_or_assign("version", [status](context&, const planar::cliapp::parsed_args&) -> handler_outcome {
      return exit_status{.code = status};
    });
  });
}

} // namespace

TEST_CASE("planar-agent: a handler's exit status 7 is the process exit code", "[cmd][agent][exit][passthrough]") {
  auto const got = version_returning(7);
  CHECK(got.code == 7);
  CHECK(got.out.empty());
  CHECK(got.err.empty());
}

TEST_CASE("planar-agent: pass-through statuses that collide with this binary's own codes are returned verbatim",
          "[cmd][agent][exit][passthrough]") {
  // 1 is this binary's parse-failure code, 2 its user-input code, 125 the
  // queue's own failure code (decision 1188). A child may exit with any of
  // them; the process must too, and must not dress it up as a refusal.
  for (int const status : {0, 1, 2, 5, 64, 125, 127, 128 + 9, 255}) {
    INFO("status " << status);
    auto const got = version_returning(status);
    CHECK(got.code == status);
    CHECK(got.out.empty());
    CHECK(got.err.empty());
  }
}

TEST_CASE("planar-agent: a pass-through status never writes the domain-error formats", "[cmd][agent][exit][passthrough]") {
  // `context list` takes `--json`, so a domain error from it writes BOTH the
  // stderr line and the stdout JSON envelope. A pass-through 2 must write
  // neither, though 2 is exactly the code an `invalid_input` error maps to.
  std::vector<std::string> const argv{"context", "list", "--run", "1", "--json"};

  auto const passed = dispatch_with("nofmt", argv, [](handler_table& table) {
    table.insert_or_assign(
        "context list", [](context&, const planar::cliapp::parsed_args&) -> handler_outcome { return exit_status{.code = 2}; });
  });
  CHECK(passed.code == 2);
  CHECK(passed.out.empty());
  CHECK(passed.err.empty());

  // The same leaf refusing with a domain error that maps to 2 writes both:
  // the contrast that shows the case above is not vacuous.
  auto const refused = dispatch_with("fmt", argv, [](handler_table& table) {
    table.insert_or_assign("context list", [](context&, const planar::cliapp::parsed_args&) -> handler_outcome {
      return handler_result{std::unexpected(error_from_body(domain_error_kind::invalid_input, "context list: BadRun"))};
    });
  });
  CHECK(refused.code == 2);
  CHECK(refused.err == "error: context list: BadRun\n");
  CHECK(refused.out == "{\"error\":{\"verb\":\"context list\",\"tag\":\"BadRun\"}}\n");
}

TEST_CASE("planar-agent: a domain error still exits with its mapped code beside the pass-through path",
          "[cmd][agent][exit][passthrough]") {
  auto const got = dispatch_with("domain", {"version"}, [](handler_table& table) {
    table.insert_or_assign("version", [](context&, const planar::cliapp::parsed_args&) -> handler_outcome {
      return handler_result{std::unexpected(error_from_body(domain_error_kind::scope_mismatch, "version: ScopeMismatch"))};
    });
  });
  CHECK(got.code == 5);
  CHECK(got.err == "error: version: ScopeMismatch\n");

  // A handler that succeeds the ordinary way still exits 0.
  auto const ok = dispatch_with("ok", {"version"}, [](handler_table& table) {
    table.insert_or_assign("version",
                           [](context&, const planar::cliapp::parsed_args&) -> handler_outcome { return handler_result{}; });
  });
  CHECK(ok.code == 0);
  CHECK(ok.err.empty());
}

TEST_CASE("planar-agent: a pass-through status outside 0..255 exits with the internal-error code",
          "[cmd][agent][exit][passthrough]") {
  // A process cannot report such a status: the kernel keeps the low eight
  // bits, so 256 would read as a clean 0. It is a handler bug, reported as
  // one rather than truncated.
  CHECK(planar::cmd::agent::exit_internal_error == 125);
  for (int const status : {-1, 256, 263, 1000, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
    INFO("status " << status);
    auto const got = version_returning(status);
    CHECK(got.code == planar::cmd::agent::exit_internal_error);
    CHECK(got.out.empty());
    CHECK(got.err == std::format("error: version: exit status {} is outside 0..255\n", status));
  }
}

TEST_CASE("planar-agent: the process exits with the code dispatch returns", "[cmd][agent][exit][passthrough]") {
  // `main` returns `run`'s value. Proven black-box for three distinct codes
  // by requiring the built binary's exit status to equal the in-process
  // `run` for the same argv. Pinned values guard against both sides
  // drifting together. `claim` against an unmigrated scratch database is
  // refused with the schema-version code, 7, on both sides.
  struct row {
    std::vector<std::string> argv;
    int                      code;
  };
  std::vector<row> const rows{
      {.argv = {"version"}, .code = 0},
      {.argv = {"nosuchverb"}, .code = 1},
      {.argv = {"claim", "--entity", "task:abc"}, .code = 7},
  };
  auto const arena = planar::cmd::parity::make_arena("passthrough_main");
  int        n     = 0;
  for (auto const& r : rows) {
    INFO("argv " << r.argv.front());
    auto const in_process = dispatch_with("main", r.argv, [](handler_table&) {});
    auto const binary =
        planar::cmd::parity::run_pinned(std::filesystem::path{PLANAR_CPP_BIN}, r.argv, arena.cpp_root, std::format("m{}", n++));
    CHECK(in_process.code == r.code);
    CHECK(binary.code == in_process.code);
  }
}
