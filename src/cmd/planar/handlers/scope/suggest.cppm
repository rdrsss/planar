/// @file suggest.cppm
/// @brief CLI declaration for `scope suggest`.
export module planar.cmd.planar.handlers.scope.suggest;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scope_cli {
/// @brief Register the suggest CLI node.
/// @param scope Input scope.
/// @return Registered CLI node.
export auto attach_suggest(CLI::App* scope) -> CLI::App* {
  CLI::App* suggest = scope->add_subcommand("suggest", "Suggest scope associations based on cwd.");
  add_json(*suggest, "Emit machine-readable JSON instead of text");
  return suggest;
}
} // namespace planar::cmd::handlers::scope_cli
