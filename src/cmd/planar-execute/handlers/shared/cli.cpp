/// @file cli.cpp
/// @brief Implementation of `planar.cmd.planar_execute.cli`.

module planar.cmd.planar_execute.cli;

import std;

namespace planar::cmd::execute {

auto usage_text() -> std::string_view {
  // Transcribed from zig/src/cmd/planar-execute/main.zig's `usage`
  // multiline literal and then diffed against the oracle's actual stderr
  // bytes. The em dash on the first line is the oracle's own UTF-8. Post-
  // oracle additions: the `schema` line (task 6486, D18), `--engine` and
  // `profile show` (plan 1033 task 6485, D5), `submit` (task 6504), and
  // `status`/`cancel`/`host status` (task 6506).
  return "planar-execute — deterministic, spawn-free Lua workflow engine.\n"
         "\n"
         "Usage:\n"
         "  planar-execute run <workflow.lua> --phase <name> [--args <json>]\n"
         "                     [--worktree <dir>] [--sandbox-root <dir>]\n"
         "                     [--engine <embedded|centurion>]\n"
         "  planar-execute profile show [--profile <name>] [--json]\n"
         "  planar-execute submit <bundle> [--input <json>] [--profile <name>]\n"
         "  planar-execute status [<run-id>] [--profile <name>] [--json]\n"
         "  planar-execute cancel <run-id> [--profile <name>] [--json]\n"
         "  planar-execute host status|drain|stop [--profile <name>] [--json]\n"
         "  planar-execute follow <run-id> [--from <cursor>] [--profile <name>]\n"
         "  planar-execute schema\n"
         "\n"
         "Loads the workflow in the sandbox, registers the deterministic host\n"
         "surface (cli/git/fs/flow/ctx), calls the named phase, and prints the\n"
         "workflow's flow.result(table) payload as JSON on stdout.\n";
}

auto parse_run_id_args(std::span<const std::string> args, bool run_id_required) -> std::optional<run_id_args> {
  run_id_args parsed;
  bool        saw_run_id = false;
  for (std::size_t index = 0; index < args.size(); ++index) {
    auto const& token = args[index];
    if (token == "--profile") {
      if (index + 1 >= args.size()) {
        return std::nullopt;
      }
      parsed.profile = args[++index];
    } else if (token == "--json") {
      parsed.json = true;
    } else if (token == "--from") {
      if (index + 1 >= args.size()) {
        return std::nullopt;
      }
      const auto&   value        = args[++index];
      std::uint64_t cursor       = 0;
      const auto [stop, failure] = std::from_chars(value.data(), value.data() + value.size(), cursor);
      // The WHOLE token must be the number: "12abc" is a typo, and accepting
      // its prefix would silently follow from somewhere the caller never named.
      if (failure != std::errc{} || stop != value.data() + value.size()) {
        return std::nullopt;
      }
      parsed.from = cursor;
    } else if (token.starts_with("-")) {
      return std::nullopt;
    } else if (saw_run_id) {
      return std::nullopt;
    } else {
      parsed.run_id = token;
      saw_run_id    = true;
    }
  }
  if (parsed.profile.empty() || (run_id_required && parsed.run_id.empty())) {
    return std::nullopt;
  }
  return parsed;
}

auto parse_submit_args(std::span<const std::string> args) -> std::optional<submit_args> {
  submit_args parsed;
  bool        saw_bundle = false;
  for (std::size_t index = 0; index < args.size(); ++index) {
    auto const& token = args[index];
    auto const  value = [&](std::string& target) {
      if (index + 1 >= args.size()) {
        return false;
      }
      target = args[++index];
      return true;
    };
    if (token == "--input") {
      if (!value(parsed.input)) {
        return std::nullopt;
      }
    } else if (token == "--profile") {
      if (!value(parsed.profile)) {
        return std::nullopt;
      }
    } else if (token.starts_with("-")) {
      return std::nullopt;
    } else if (saw_bundle) {
      // A second positional is a typo, not a second bundle.
      return std::nullopt;
    } else {
      parsed.bundle = token;
      saw_bundle    = true;
    }
  }
  // `saw_bundle` is deliberately NOT re-checked here: it can only be true
  // when a token was assigned, and an empty token is refused by the same
  // clause, so testing both would be one condition no input can separate.
  if (parsed.bundle.empty() || parsed.input.empty() || parsed.profile.empty()) {
    return std::nullopt;
  }
  return parsed;
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

auto parse_profile_args(std::span<const std::string> args) -> std::optional<profile_args> {
  if (args.empty() || args[0] != "show") {
    return std::nullopt;
  }
  profile_args out;
  bool         saw_json    = false;
  bool         saw_profile = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--json" && !saw_json) {
      saw_json = out.json = true;
    } else if (args[i] == "--profile" && !saw_profile && i + 1 < args.size()) {
      saw_profile = true;
      out.name    = args[++i];
    } else {
      return std::nullopt;
    }
  }
  return out;
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
  if (token == "submit") {
    return verb::submit;
  }
  if (token == "status") {
    return verb::status;
  }
  if (token == "cancel") {
    return verb::cancel;
  }
  if (token == "host") {
    return verb::host;
  }
  if (token == "follow") {
    return verb::follow;
  }
  return verb::unknown;
}

} // namespace planar::cmd::execute
