/// @file init.cppm
/// @brief CLI declaration for `config init`.
export module planar.cmd.planar.handlers.config.init;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::config_cli {
/// @brief Register the init CLI node.
/// @param config Input config.
/// @return Registered CLI node.
export auto attach_init(CLI::App* config) -> CLI::App* {
  config->add_subcommand("init", "Initialize the configuration file.");
  return config->get_subcommand("init");
}
} // namespace planar::cmd::handlers::config_cli
