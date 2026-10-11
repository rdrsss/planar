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
  add_string(*list, "--vendor", "Vendor name; accepted but not read by this verb");
  add_string(*list, "--note", "Free-form note; accepted but not read by this verb");
  add_json(*list, "Emit machine-readable JSON instead of text");
  add_string(*list, "--status", "Filter by handoff status: pending, validated, consumed, abandoned");
  return list;
}
} // namespace planar::cmd::handlers::handoff_cli
