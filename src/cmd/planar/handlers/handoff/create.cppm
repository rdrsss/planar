/// @file create.cppm
/// @brief CLI declaration for `handoff create`.
export module planar.cmd.planar.handlers.handoff.create;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
export auto attach_create(CLI::App* handoff) -> CLI::App* {
  CLI::App* create = handoff->add_subcommand("create", "Create a handoff from an existing snapshot.");
  add_string(*create, "--vendor");
  add_string(*create, "--note");
  add_json(*create);
  add_positional(*create, "snapshot-id");
  return create;
}
} // namespace planar::cmd::handlers::handoff_cli
