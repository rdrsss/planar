/// @file validate.cppm
/// @brief CLI declaration for `config validate`.
export module planar.cmd.planar.handlers.config.validate;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::config_cli {
/// @brief Register the validate CLI node.
/// @param config Input config.
/// @return Registered CLI node.
export auto attach_validate(CLI::App* config) -> CLI::App* {
  config->add_subcommand("validate", "Validate configuration file syntax.");
  return config->get_subcommand("validate");
}
} // namespace planar::cmd::handlers::config_cli
