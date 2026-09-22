/// @file list.cppm
/// @brief CLI declaration for `templates list`.
export module planar.cmd.planar.handlers.templates.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::templates_cli {
export auto attach_list(CLI::App* templates) -> CLI::App* {
  CLI::App* list = templates->add_subcommand("list", "List available templates.");
  add_string(*list, "--system");
  add_string(*list, "--set");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::templates_cli
