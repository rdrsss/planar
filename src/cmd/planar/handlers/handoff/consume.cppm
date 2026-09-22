/// @file consume.cppm
/// @brief CLI declaration for `handoff consume`.
export module planar.cmd.planar.handlers.handoff.consume;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
export auto attach_consume(CLI::App* handoff) -> CLI::App* {
  CLI::App* consume = handoff->add_subcommand("consume", "Mark a handoff as consumed.");
  add_string(*consume, "--vendor");
  add_string(*consume, "--note");
  add_json(*consume);
  add_int(*consume, "--session");
  add_positional(*consume, "handoff-id");
  return consume;
}
} // namespace planar::cmd::handlers::handoff_cli
