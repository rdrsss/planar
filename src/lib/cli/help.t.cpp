// @file help.t.cpp
// @brief Unit tests for `planar.cli.help::render_help` (plan 996, task
// cpp-cli-tree-parity).
//
// Rather than transcribing a captured help-text snapshot into the test
// body (a hand-typed transcript can silently drift or contain a
// transcription slip), the `[parity]` cases below run the ACTUAL
// reference binaries live and diff their `--help` stdout byte-for-byte
// against `render_help`'s output for the equivalently-shaped modeled
// leaf — the task brief's "derive the expected value by RUNNING
// ./zig/zig-out/bin/planar ... and capturing it" applied literally, not
// just at authoring time. SKIPs (rather than fails) when the reference
// binary is not built, matching src/lib/db/migrate.t.cpp's established
// pattern.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.cli;

namespace {

using planar::cli::cmd;
using planar::cli::flag;
using planar::cli::positional;

// Same modeled subset as parser.t.cpp (see that file's header comment for
// why these three leaves were chosen); duplicated here because each
// `*.t.cpp` compiles as its own translation unit with no shared test
// header in this module.
auto make_planar_root() -> cmd {
  cmd add{
      .name = "add",
      .desc = "Create a new task.",
      .flags =
          {
              flag{.long_name = "--body"},
              flag{.long_name = "--scope"},
              flag{.long_name = "--next-action"},
              flag{.long_name = "--due"},
              flag{.long_name = "--plan", .value_kind = planar::cli::kind::integer},
              flag{.long_name = "--parent", .value_kind = planar::cli::kind::integer},
              flag{.long_name = "--slug"},
              flag{.long_name = "--priority", .value_kind = planar::cli::kind::integer, .default_value = std::int64_t{100}},
              flag{.long_name = "--editor", .value_kind = planar::cli::kind::boolean, .default_value = true},
              flag{.long_name = "--no-auto-promote", .value_kind = planar::cli::kind::boolean, .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
      .positionals = {positional{.name = "title", .required = true}},
  };
  cmd done{
      .name = "done",
      .desc = "Mark a task as done (single-arg form; Go supports variadic).",
      .flags =
          {
              flag{.long_name = "--scope"},
              flag{.long_name     = "--force",
                   .desc          = "Override active-claim guard and flip status anyway.",
                   .value_kind    = planar::cli::kind::boolean,
                   .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
      .positionals = {positional{.name = "task-id", .required = true}},
  };
  cmd task{
      .name = "task",
      .desc = "Manage tasks.",
      .cmds = {std::move(add), std::move(done)},
  };
  return cmd{.name = "planar", .cmds = {std::move(task)}};
}

auto make_planar_agent_root() -> cmd {
  cmd fail{
      .name = "fail",
      .desc = "Atomically fail the work session: task \xe2\x86\x92 todo, claim \xe2\x86\x92 aborted.",
      .flags =
          {
              flag{.long_name = "--claim", .desc = "Claim token returned by pull/claim", .required = true},
              flag{.long_name = "--reason", .desc = "Failure reason recorded on the claim and action", .required = true},
              flag{
                  .long_name     = "--category",
                  .desc          = "Closed failure category (default: unknown)",
                  .value_kind    = planar::cli::kind::choice,
                  .choices       = {"usage_limit", "context_limit", "output_limit", "tool_failure", "validation", "unknown"},
                  .default_value = std::string("unknown"),
              },
              flag{.long_name     = "--no-locality-probe",
                   .desc          = "Skip the git locality probe and commit collection",
                   .value_kind    = planar::cli::kind::boolean,
                   .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
  };
  return cmd{.name = "planar-agent", .cmds = {std::move(fail)}};
}

/// @brief Environment prefix that pins the reference binary to a throwaway
/// database.
///
/// The Zig runtime resolves `$PLANAR_DB` and otherwise falls back to
/// `~/.planar/planar.db`, applying any pending migrations **automatically** on
/// first use. Shelling the reference binary with the inherited environment
/// therefore points it at the operator's live database — and the moment a
/// migration lands on this branch, running `ctest` would migrate that database
/// past the version every installed binary supports, locking out every other
/// agent on the machine. Isolating here keeps the suite honest about the
/// repository rule that a from-source binary never touches the real database.
auto scratch_env_prefix() -> std::string {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cli_help_env_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  return std::format("PLANAR_DB='{}' PLANAR_HOME='{}' PLANAR_CONFIG_PATH='{}' ", (root / "planar.db").string(),
                     (root / "home").string(), (root / "config.toml").string());
}

/// @brief Run `bin arg1 arg2 ...`, redirecting stdout+stderr to a scratch
/// file, and return its contents. Caller checks the binary exists first
/// (SKIP otherwise) — mirrors migrate.t.cpp's `std::system` + redirect
/// idiom rather than introducing a new process-capture primitive.
auto capture_stdout(std::string const& bin, std::vector<std::string> const& args) -> std::string {
  auto const out_path =
      std::filesystem::temp_directory_path() /
      std::format("planar_cli_help_capture_{}.txt", std::chrono::steady_clock::now().time_since_epoch().count());
  std::string cmd_str = scratch_env_prefix() + bin;
  for (auto const& a : args) {
    cmd_str += " '" + a + "'";
  }
  cmd_str += " > " + out_path.string() + " 2>&1";
  std::system(cmd_str.c_str());

  std::ifstream   in(out_path, std::ios::binary);
  std::string     contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::error_code ec;
  std::filesystem::remove(out_path, ec);
  return contents;
}

} // namespace

TEST_CASE("render_help: leaf help renders USAGE, FLAGS, and POSITIONAL ARGUMENTS", "[help][nested]") {
  auto       root = make_planar_root();
  auto const text = planar::cli::render_help(root, std::vector<std::string>{"task", "add"});
  CHECK(text.starts_with("task add\n\n  Create a new task.\n\nUSAGE:\n  task add [flags] <title>\n"));
  CHECK(text.find("FLAGS:\n") != std::string::npos);
  CHECK(text.find("--priority            (int) default=100") != std::string::npos);
  CHECK(text.find("--editor              (bool) default=true") != std::string::npos);
  CHECK(text.find("POSITIONAL ARGUMENTS:\n  <title>         (string)\n") != std::string::npos);
  // FLAGS section precedes POSITIONAL ARGUMENTS, matching etcli's fixed
  // section order.
  CHECK(text.find("FLAGS:") < text.find("POSITIONAL ARGUMENTS:"));
}

TEST_CASE("render_help: a parent (group) node without a resolved leaf lists its COMMANDS", "[help][nested]") {
  auto       root = make_planar_root();
  auto const text = planar::cli::render_help(root, std::vector<std::string>{"task"});
  CHECK(text.starts_with("task\n\n  Manage tasks.\n\nUSAGE:\n  task <command>\n"));
  CHECK(text.find("COMMANDS:\n") != std::string::npos);
  CHECK(text.find("add             Create a new task.\n") != std::string::npos);
  CHECK(text.find("done            Mark a task as done") != std::string::npos);
  // A childless-flags group node has no FLAGS section at all.
  CHECK(text.find("FLAGS:") == std::string::npos);
}

TEST_CASE("render_help: choice flag kind renders as a pipe-joined value list with a quoted default", "[help][choice]") {
  auto       root = make_planar_agent_root();
  auto const text = planar::cli::render_help(root, std::vector<std::string>{"fail"});
  CHECK(text.find("--category            (usage_limit|context_limit|output_limit|tool_failure|validation|unknown) "
                  "default=\"unknown\" \xe2\x80\x94 Closed failure category (default: unknown)") != std::string::npos);
  CHECK(text.find("--claim               (string) required \xe2\x80\x94 Claim token returned by pull/claim") !=
        std::string::npos);
}

TEST_CASE("parity: rendered help matches the reference planar binary byte-for-byte", "[help][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_PLANAR_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }
  auto root = make_planar_root();

  auto const reference_add = capture_stdout(zig_bin.string(), {"task", "add", "--help"});
  auto const rendered_add  = planar::cli::render_help(root, std::vector<std::string>{"task", "add"});
  CHECK(rendered_add == reference_add);

  auto const reference_done = capture_stdout(zig_bin.string(), {"task", "done", "--help"});
  auto const rendered_done  = planar::cli::render_help(root, std::vector<std::string>{"task", "done"});
  CHECK(rendered_done == reference_done);
}

TEST_CASE("parity: agent fail help matches the reference planar-agent binary byte-for-byte", "[help][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_PLANAR_AGENT_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }
  auto       root      = make_planar_agent_root();
  auto const reference = capture_stdout(zig_bin.string(), {"fail", "--help"});
  auto const rendered  = planar::cli::render_help(root, std::vector<std::string>{"fail"});
  CHECK(rendered == reference);
}
