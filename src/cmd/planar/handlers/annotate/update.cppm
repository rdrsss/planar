/// @file update.cppm
/// @brief CLI declaration for `annotate update`.
export module planar.cmd.planar.handlers.annotate.update;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the update CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_update(CLI::App& annotate) -> CLI::App* {
  CLI::App* update = annotate.add_subcommand("update", "Update an annotation.");
  add_string(*update, "--title", k_undocumented);
  add_string(*update, "--slug", k_undocumented);
  add_string(*update, "--body", k_undocumented);
  add_string(*update, "--status", k_undocumented);
  add_int(*update, "--plan", k_undocumented);
  add_int(*update, "--task", k_undocumented);
  add_string(*update, "--scope", k_undocumented);
  add_json(*update, k_undocumented);
  add_positional(*update, "annotation-id", k_undocumented);
  return update;
}
} // namespace planar::cmd::handlers::annotate_cli
