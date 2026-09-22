/// @file pop.cppm
/// @brief CLI declaration for `scope pop`.
export module planar.cmd.planar.handlers.scope.pop;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scope_cli {
export auto attach_pop(CLI::App* scope) -> CLI::App* {
  CLI::App* pop = scope->add_subcommand("pop", "Removed in plan 153 M5 — see `planar scope show`.");
  set_allow_extras(*pop);
  return pop;
}
} // namespace planar::cmd::handlers::scope_cli
