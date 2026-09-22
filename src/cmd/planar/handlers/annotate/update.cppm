/// @file update.cppm
/// @brief CLI declaration for `annotate update`.
export module planar.cmd.planar.handlers.annotate.update;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
export auto attach_update(CLI::App& annotate) -> CLI::App* {
  CLI::App* update = annotate.add_subcommand("update", "Update an annotation.");
  add_string(*update, "--title");
  add_string(*update, "--slug");
  add_string(*update, "--body");
  add_string(*update, "--status");
  add_int(*update, "--plan");
  add_int(*update, "--task");
  add_string(*update, "--scope");
  add_json(*update);
  add_positional(*update, "annotation-id");
  return update;
}
} // namespace planar::cmd::handlers::annotate_cli
