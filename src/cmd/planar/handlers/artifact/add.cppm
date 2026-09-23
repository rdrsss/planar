/// @file add.cppm
/// @brief CLI declaration for `artifact add`.
export module planar.cmd.planar.handlers.artifact.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the add CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_add(CLI::App& artifact) -> CLI::App* {
  CLI::App* add = artifact.add_subcommand("add", "Register a new artifact.");
  add_string(*add, "--body");
  add_string_required(*add, "--kind");
  add_string(*add, "--from-file");
  add_string(*add, "--source-path");
  add_string(*add, "--scope");
  add_string_default(*add, "--status", "draft");
  add_int(*add, "--plan");
  add_bool_default_true(*add, "--editor");
  add_json(*add);
  add_positional(*add, "title");
  return add;
}
} // namespace planar::cmd::handlers::artifact_cli
