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
  add_string(*abandon, "--vendor");
  add_string(*abandon, "--note");
  add_json(*abandon);
  add_string(*abandon, "--reason");
  add_positional(*abandon, "handoff-id");
  return abandon;
}
} // namespace planar::cmd::handlers::handoff_cli
