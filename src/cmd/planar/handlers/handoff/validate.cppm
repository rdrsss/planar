/// @file validate.cppm
/// @brief CLI declaration for `handoff validate`.
export module planar.cmd.planar.handlers.handoff.validate;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the validate CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_validate(CLI::App* handoff) -> CLI::App* {
  CLI::App* validate = handoff->add_subcommand("validate", "Validate a pending handoff.");
  add_string(*validate, "--vendor", "Vendor name; accepted but not read by this verb");
  add_string(*validate, "--note", "Free-form note; accepted but not read by this verb");
  add_json(*validate, "Emit machine-readable JSON instead of text");
  add_positional(*validate, "handoff-id", "Handoff id");
  return validate;
}
} // namespace planar::cmd::handlers::handoff_cli
