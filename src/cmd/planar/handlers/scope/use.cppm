/// @file use.cppm
/// @brief CLI declaration for `scope use`.
export module planar.cmd.planar.handlers.scope.use;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scope_cli {
export auto attach_use(CLI::App* scope) -> CLI::App* {
  CLI::App* use = scope->add_subcommand("use", "Removed in plan 153 M5 — see `planar scope show`.");
  set_allow_extras(*use);
  add_positional_optional(*use, "slug");
  return use;
}
} // namespace planar::cmd::handlers::scope_cli
