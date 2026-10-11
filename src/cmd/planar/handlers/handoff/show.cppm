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
  add_string(*show, "--vendor", "Vendor name; accepted but not read by this verb");
  add_string(*show, "--note", "Free-form note; accepted but not read by this verb");
  add_json(*show, "Emit machine-readable JSON instead of text");
  add_positional(*show, "handoff-id", "Handoff id");
  return show;
}
} // namespace planar::cmd::handlers::handoff_cli
