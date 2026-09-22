/// @file init.cppm
/// @brief CLI declaration for `templates init`.
export module planar.cmd.planar.handlers.templates.init;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
export auto attach_init(CLI::App* templates) -> CLI::App* {
  CLI::App* init = templates->add_subcommand("init", "Extract default templates to disk.");
  add_bool(*init, "--force");
  add_json(*init);
  return init;
}
} // namespace planar::cmd::handlers::templates_cli
