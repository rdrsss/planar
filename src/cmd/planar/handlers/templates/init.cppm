/// @file init.cppm
/// @brief CLI declaration for `templates init`.
export module planar.cmd.planar.handlers.templates.init;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
/// @brief Register the init CLI node.
/// @param templates Input templates.
/// @return Registered CLI node.
export auto attach_init(CLI::App* templates) -> CLI::App* {
  CLI::App* init = templates->add_subcommand("init", "Extract default templates to disk.");
  add_bool(*init, "--force", "Overwrite template files that already exist on disk");
  add_json(*init, "Emit machine-readable JSON instead of text");
  return init;
}
} // namespace planar::cmd::handlers::templates_cli
