/// @file unlink.cppm
/// @brief CLI declaration for `local unlink`.
export module planar.cmd.planar.handlers.local.unlink;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::local_cli {
/// @brief Register the unlink CLI node.
/// @param local Input local.
/// @return Registered CLI node.
export auto attach_unlink(CLI::App* local) -> CLI::App* {
  CLI::App* unlink = local->add_subcommand("unlink", "Remove the projected copies recorded for a local source.");
  add_bool(*unlink, "--purge", "Also delete the source file from the local sandbox");
  add_json(*unlink, "Emit machine-readable JSON instead of text");
  add_positional(*unlink, "name", "Name of the skill or agent to unlink");
  return unlink;
}
} // namespace planar::cmd::handlers::local_cli
