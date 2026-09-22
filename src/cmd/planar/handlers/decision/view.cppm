/// @file view.cppm
/// @brief CLI declaration for `decision view`.
export module planar.cmd.planar.handlers.decision.view;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
export auto attach_view(CLI::App& decision) -> CLI::App* {
  CLI::App* view = decision.add_subcommand("view", "View decision's workbench file.");
  add_positional(*view, "decision-id");
  return view;
}
} // namespace planar::cmd::handlers::decision_cli
