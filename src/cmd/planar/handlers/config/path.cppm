/// @file path.cppm
/// @brief CLI declaration for `config path`.
export module planar.cmd.planar.handlers.config.path;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::config_cli {
/// @brief Register the path CLI node.
/// @param config Input config.
/// @return Registered CLI node.
export auto attach_path(CLI::App* config) -> CLI::App* {
  config->add_subcommand("path", "Show the configuration file path.");
  return config->get_subcommand("path");
}
} // namespace planar::cmd::handlers::config_cli
