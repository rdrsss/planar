/// @file list.cppm
/// @brief CLI declaration for `workbench list`.
export module planar.cmd.planar.handlers.workbench.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
export auto attach_list(CLI::App& workbench) -> CLI::App* {
  CLI::App* list = workbench.add_subcommand("list", "List features with workbench trees.");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::workbench_cli
