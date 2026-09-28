/// @file clear.cppm
/// @brief CLI declaration for `scope clear`.
export module planar.cmd.planar.handlers.scope.clear;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scope_cli {
/// @brief Register the clear CLI node.
/// @param scope Input scope.
/// @return Registered CLI node.
export auto attach_clear(CLI::App* scope) -> CLI::App* {
  CLI::App* clear = scope->add_subcommand("clear", "Removed in plan 153 M5 — see `planar scope show`.");
  set_allow_extras(*clear);
  return clear;
}
} // namespace planar::cmd::handlers::scope_cli
