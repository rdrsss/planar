/// @file command.cppm
/// @brief Dispatch the manually parsed planar-execute run command.
module;
export module planar.cmd.planar_execute.handlers.run.command;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.engine;
import planar.cmd.planar_execute.selector;
import planar.cmd.planar_execute.handlers.shared.engine_env;

namespace planar::cmd::execute::handlers::run {
/// @brief Execute this command.
/// @param args Input args.
/// @param out Input out.
/// @param err Input err.
/// @return Process exit code.
export auto execute(std::span<const std::string> args, std::ostream& out, std::ostream& err) -> int {
  auto const parsed = parse_run_args(args);
  if (!parsed.has_value()) {
    err << usage_text();
    return 2;
  }
  auto const flag_value = parsed->engine.empty() ? std::nullopt : std::optional<std::string_view>{parsed->engine};
  auto const choice     = resolve_engine(flag_value, shared::engine_env(), sibling_config_reader(executable_dir()));
  if (!choice.has_value()) {
    err << "planar-execute: " << choice.error() << '\n';
    return 1;
  }
  if (choice->engine == engine_kind::centurion) {
    err << "planar-execute: engine 'centurion' is not available yet (" << choice->source
        << "); the Centurion host lands in plan 1033 M2\n";
    return 1;
  }
  switch (run_workflow(*parsed, out, err)) {
  case run_outcome::ok:
    return 0;
  case run_outcome::load_failed:
  case run_outcome::engine_failed:
    return 1;
  }
  return 1;
}
} // namespace planar::cmd::execute::handlers::run
