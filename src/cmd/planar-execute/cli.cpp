/// @file cli.cpp
/// @brief Implementation of `planar.cmd.planar_execute.cli`.

module planar.cmd.planar_execute.cli;

import std;

namespace planar::cmd::execute {

auto usage_text() -> std::string_view {
  // Transcribed from zig/src/cmd/planar-execute/main.zig's `usage`
  // multiline literal and then diffed against the oracle's actual stderr
  // bytes. The em dash on the first line is the oracle's own UTF-8. Post-
  // oracle additions: the `schema` line (task 6486, D18), and `--engine` and
  // `profile show` (plan 1033 task 6485, D5).
  return "planar-execute — deterministic, spawn-free Lua workflow engine.\n"
         "\n"
         "Usage:\n"
         "  planar-execute run <workflow.lua> --phase <name> [--args <json>]\n"
         "                     [--worktree <dir>] [--sandbox-root <dir>]\n"
         "                     [--engine <embedded|centurion>]\n"
         "  planar-execute profile show [--json]\n"
         "  planar-execute schema\n"
         "\n"
         "Loads the workflow in the sandbox, registers the deterministic host\n"
         "surface (cli/git/fs/flow/ctx), calls the named phase, and prints the\n"
         "workflow's flow.result(table) payload as JSON on stdout.\n";
}

auto parse_run_args(std::span<const std::string> args) -> std::optional<run_args> {
  std::optional<std::string> workflow;
  std::optional<std::string> phase;
  std::string                args_json;
  std::string                worktree;
  std::string                sandbox_root;
  std::string                engine;

  // Index-based rather than range-based precisely because a flag CONSUMES
  // the following token; `++i` inside the body is the mechanism, and a
  // missing value (i past the end) is a usage failure, not a silent empty.
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto const& token      = args[i];
    auto const  take_value = [&](std::string& sink) -> bool {
      ++i;
      if (i >= args.size()) {
        return false;
      }
      sink = args[i];
      return true;
    };

    if (token == "--phase") {
      std::string value;
      if (!take_value(value)) {
        return std::nullopt;
      }
      phase = std::move(value);
    } else if (token == "--args") {
      if (!take_value(args_json)) {
        return std::nullopt;
      }
    } else if (token == "--worktree") {
      if (!take_value(worktree)) {
        return std::nullopt;
      }
    } else if (token == "--sandbox-root") {
      if (!take_value(sandbox_root)) {
        return std::nullopt;
      }
    } else if (token == "--engine") {
      // Validated HERE, so a misspelt engine is a usage failure (exit 2)
      // like every other malformed flag, never a dispatch-time surprise.
      if (!take_value(engine) || (engine != "embedded" && engine != "centurion")) {
        return std::nullopt;
      }
    } else if (token.starts_with("--")) {
      // An unrecognised long flag is a REFUSAL, not something to ignore.
      return std::nullopt;
    } else if (!workflow.has_value()) {
      workflow = token;
    } else {
      // A second bare positional is a refusal too — it does not overwrite
      // the first.
      return std::nullopt;
    }
  }

  if (!workflow.has_value() || !phase.has_value()) {
    return std::nullopt;
  }
  return run_args{.workflow     = std::move(*workflow),
                  .phase        = std::move(*phase),
                  .args_json    = std::move(args_json),
                  .worktree     = std::move(worktree),
                  .sandbox_root = std::move(sandbox_root),
                  .engine       = std::move(engine)};
}

auto parse_profile_args(std::span<const std::string> args) -> std::optional<bool> {
  if (args.empty() || args[0] != "show") {
    return std::nullopt;
  }
  if (args.size() == 1) {
    return false;
  }
  if (args.size() == 2 && args[1] == "--json") {
    return true;
  }
  return std::nullopt;
}

auto classify(std::span<const std::string> argv) -> verb {
  if (argv.size() < 2) {
    return verb::none;
  }
  auto const& token = argv[1];
  // `run` is tested FIRST, matching the Zig original — see this module's
  // header for why that ordering is observable.
  if (token == "run") {
    return verb::run;
  }
  if (token == "--help" || token == "-h" || token == "help") {
    return verb::help;
  }
  if (token == "schema") {
    return verb::schema;
  }
  if (token == "profile") {
    return verb::profile;
  }
  return verb::unknown;
}

} // namespace planar::cmd::execute
