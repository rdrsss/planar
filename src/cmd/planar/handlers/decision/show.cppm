/// @file show.cppm
/// @brief CLI declaration for `decision show`.
export module planar.cmd.planar.handlers.decision.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
export auto attach_show(CLI::App& decision) -> CLI::App* {
  CLI::App* show = decision.add_subcommand("show", "Show a decision's details.");
  add_json(*show);
  add_positional(*show, "decision-id");
  return show;
}
} // namespace planar::cmd::handlers::decision_cli
