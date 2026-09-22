/// @file capabilities.cppm
/// @brief CLI declaration for `annotate capabilities`.
export module planar.cmd.planar.handlers.annotate.capabilities;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
export auto attach_capabilities(CLI::App& annotate) -> CLI::App* {
  CLI::App* capabilities = annotate.add_subcommand("capabilities", "Describe annotation read and command support.");
  add_json(*capabilities);
  return capabilities;
}
} // namespace planar::cmd::handlers::annotate_cli
