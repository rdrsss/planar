/// @file list.cppm
/// @brief CLI declaration for `handoff list`.
export module planar.cmd.planar.handlers.handoff.list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the list CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_list(CLI::App* handoff) -> CLI::App* {
  CLI::App* list = handoff->add_subcommand("list", "List handoffs.");
  add_string(*list, "--vendor");
  add_string(*list, "--note");
  add_json(*list);
  add_string(*list, "--status");
  return list;
}
} // namespace planar::cmd::handlers::handoff_cli
