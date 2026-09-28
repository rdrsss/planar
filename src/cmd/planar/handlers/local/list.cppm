/// @file list.cppm
/// @brief CLI declaration for `local list`.
export module planar.cmd.planar.handlers.local.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::local_cli {
/// @brief Register the list CLI node.
/// @param local Input local.
/// @return Registered CLI node.
export auto attach_list(CLI::App* local) -> CLI::App* {
  CLI::App* list = local->add_subcommand("list", "List locally-installed skills and agents.");
  add_string(*list, "--vendor");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::local_cli
