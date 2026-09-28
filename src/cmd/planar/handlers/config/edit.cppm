/// @file edit.cppm
/// @brief CLI declaration for `config edit`.
export module planar.cmd.planar.handlers.config.edit;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::config_cli {
/// @brief Register the edit CLI node.
/// @param config Input config.
/// @return Registered CLI node.
export auto attach_edit(CLI::App* config) -> CLI::App* {
  config->add_subcommand("edit", "Edit the configuration file in $EDITOR.");
  return config->get_subcommand("edit");
}
} // namespace planar::cmd::handlers::config_cli
