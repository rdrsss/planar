/// @file list.cppm
/// @brief CLI declaration for `templates list`.
export module planar.cmd.planar.handlers.templates.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
/// @brief Register the list CLI node.
/// @param templates Input templates.
/// @return Registered CLI node.
export auto attach_list(CLI::App* templates) -> CLI::App* {
  CLI::App* list = templates->add_subcommand("list", "List available templates.");
  add_string(*list, "--system", "Only list templates for this external system");
  add_string(*list, "--set", "Only list templates in this set");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::templates_cli
