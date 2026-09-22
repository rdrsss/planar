/// @file show.cppm
/// @brief CLI declaration for `handoff show`.
export module planar.cmd.planar.handlers.handoff.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
export auto attach_show(CLI::App* handoff) -> CLI::App* {
  CLI::App* show = handoff->add_subcommand("show", "Show a handoff's details.");
  add_string(*show, "--vendor");
  add_string(*show, "--note");
  add_json(*show);
  add_positional(*show, "handoff-id");
  return show;
}
} // namespace planar::cmd::handlers::handoff_cli
