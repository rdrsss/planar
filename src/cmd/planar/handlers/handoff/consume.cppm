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
  add_string(*consume, "--vendor", k_undocumented);
  add_string(*consume, "--note", k_undocumented);
  add_json(*consume, k_undocumented);
  add_int(*consume, "--session", k_undocumented);
  add_positional(*consume, "handoff-id", k_undocumented);
  return consume;
}
} // namespace planar::cmd::handlers::handoff_cli
