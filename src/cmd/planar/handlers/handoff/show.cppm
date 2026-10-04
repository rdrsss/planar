/// @file show.cppm
/// @brief CLI declaration for `handoff show`.
export module planar.cmd.planar.handlers.handoff.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the show CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_show(CLI::App* handoff) -> CLI::App* {
  CLI::App* show = handoff->add_subcommand("show", "Show a handoff's details.");
  add_string(*show, "--vendor", k_undocumented);
  add_string(*show, "--note", k_undocumented);
  add_json(*show, k_undocumented);
  add_positional(*show, "handoff-id", k_undocumented);
  return show;
}
} // namespace planar::cmd::handlers::handoff_cli
