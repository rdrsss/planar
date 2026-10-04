/// @file abandon.cppm
/// @brief CLI declaration for `handoff abandon`.
export module planar.cmd.planar.handlers.handoff.abandon;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the abandon CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_abandon(CLI::App* handoff) -> CLI::App* {
  CLI::App* abandon = handoff->add_subcommand("abandon", "Abandon a non-terminal handoff.");
  add_string(*abandon, "--vendor", k_undocumented);
  add_string(*abandon, "--note", k_undocumented);
  add_json(*abandon, k_undocumented);
  add_string(*abandon, "--reason", k_undocumented);
  add_positional(*abandon, "handoff-id", k_undocumented);
  return abandon;
}
} // namespace planar::cmd::handlers::handoff_cli
