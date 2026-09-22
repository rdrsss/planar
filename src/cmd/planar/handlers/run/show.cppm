/// @file show.cppm
/// @brief CLI declaration for `planar run show`.
export module planar.cmd.planar.handlers.run.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::run_cli {
export auto attach_show(CLI::App* run) -> CLI::App* {
  CLI::App* show = run->add_subcommand("show", "Show a run's full state (header + events).");
  add_json(*show);
  add_positional(*show, "run-uid");
  return show;
}
} // namespace planar::cmd::handlers::run_cli
