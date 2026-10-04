/// @file create.cppm
/// @brief CLI declaration for `handoff create`.
export module planar.cmd.planar.handlers.handoff.create;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the create CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_create(CLI::App* handoff) -> CLI::App* {
  CLI::App* create = handoff->add_subcommand("create", "Create a handoff from an existing snapshot.");
  add_string(*create, "--vendor", k_undocumented);
  add_string(*create, "--note", k_undocumented);
  add_json(*create, k_undocumented);
  add_positional(*create, "snapshot-id", k_undocumented);
  return create;
}
} // namespace planar::cmd::handlers::handoff_cli
