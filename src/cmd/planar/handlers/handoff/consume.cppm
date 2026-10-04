/// @file consume.cppm
/// @brief CLI declaration for `handoff consume`.
export module planar.cmd.planar.handlers.handoff.consume;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the consume CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_consume(CLI::App* handoff) -> CLI::App* {
  CLI::App* consume = handoff->add_subcommand("consume", "Mark a handoff as consumed.");
  add_string(*consume, "--vendor", "Vendor name; accepted but not read by this verb");
  add_string(*consume, "--note", "Free-form note; accepted but not read by this verb");
  add_json(*consume, "Emit machine-readable JSON instead of text");
  add_int(*consume, "--session", "Session id of the resuming session consuming the handoff");
  add_positional(*consume, "handoff-id", "Handoff id");
  return consume;
}
} // namespace planar::cmd::handlers::handoff_cli
