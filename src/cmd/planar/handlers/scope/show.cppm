/// @file show.cppm
/// @brief CLI declaration for `scope show`.
export module planar.cmd.planar.handlers.scope.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scope_cli {
/// @brief Register the show CLI node.
/// @param scope Input scope.
/// @return Registered CLI node.
export auto attach_show(CLI::App* scope) -> CLI::App* {
  CLI::App* show = scope->add_subcommand("show", "Show the cwd-derived scope (and any --scope override).");
  add_string(*show, "--scope");
  add_json(*show);
  return show;
}
} // namespace planar::cmd::handlers::scope_cli
